/*****************************************************************************
 * Copyright (C) 2013-2017 MulticoreWare, Inc
 *
 * Authors: Chung Shin Yee <shinyee@multicorewareinc.com>
 *          Min Chen <chenm003@163.com>
 *          Steve Borho <steve@borho.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at license @ x265.com.
 *****************************************************************************/

#include "common.h"
#include "frame.h"
#include "framedata.h"
#include "wavefront.h"
#include "param.h"

#include "encoder.h"
#include "frameencoder.h"
#include "common.h"
#include "slicetype.h"
#include "nal.h"

namespace X265_NS {
void weightAnalyse(Slice& slice, Frame& frame, x265_param& param);

FrameEncoder::FrameEncoder()
{
    m_prevOutputTime = x265_mdate();
    m_reconfigure = false;
    m_isFrameEncoder = true;
    m_threadActive = true;
    m_slicetypeWaitTime = 0;
    m_activeWorkerCount = 0;
    m_completionCount = 0;
    m_bAllRowsStop = false;
    m_vbvResetTriggerRow = -1;
    m_outStreams = NULL;
    m_backupStreams = NULL;
    m_substreamSizes = NULL;
    m_nr = NULL;
    m_tld = NULL;
    m_rows = NULL;
    m_top = NULL;
    m_param = NULL;
    m_frame = NULL;
    m_cuGeoms = NULL;
    m_ctuGeomMap = NULL;
    m_localTldIdx = 0;
    memset(&m_rce, 0, sizeof(RateControlEntry));
}

void FrameEncoder::destroy()
{
    if (m_pool)
    {
        if (!m_jpId)
        {
            int numTLD = m_pool->m_numWorkers;
            if (!m_param->bEnableWavefront)
                numTLD += m_pool->m_numProviders;
            for (int i = 0; i < numTLD; i++)
                m_tld[i].destroy();
            delete [] m_tld;
        }
    }
    else
    {
        m_tld->destroy();
        delete m_tld;
    }

    delete[] m_rows;
    delete[] m_outStreams;
    delete[] m_backupStreams;
    X265_FREE(m_sliceBaseRow);
    X265_FREE(m_sliceMaxBlockRow);
    X265_FREE(m_cuGeoms);
    X265_FREE(m_ctuGeomMap);
    X265_FREE(m_substreamSizes);
    X265_FREE(m_nr);

    m_frameFilter.destroy();

    if (m_param->bEmitHRDSEI || !!m_param->interlaceMode)
    {
        delete m_rce.picTimingSEI;
        delete m_rce.hrdTiming;
    }
}

bool FrameEncoder::init(Encoder *top, int numRows, int numCols)
{
    m_top = top;
    m_param = top->m_param;
    m_numRows = numRows;
    m_numCols = numCols;
    m_reconfigure = false;
    m_filterRowDelay = ((m_param->bEnableSAO && m_param->bSaoNonDeblocked)
                        || (!m_param->bEnableLoopFilter && m_param->bEnableSAO)) ?
                        2 : (m_param->bEnableSAO || m_param->bEnableLoopFilter ? 1 : 0);
    m_filterRowDelayCus = m_filterRowDelay * numCols;
    m_rows = new CTURow[m_numRows];
    bool ok = !!m_numRows;

    m_sliceBaseRow = X265_MALLOC(uint32_t, m_param->maxSlices + 1);
    ok &= !!m_sliceBaseRow;
    m_sliceGroupSize = (uint16_t)(m_numRows + m_param->maxSlices - 1) / m_param->maxSlices;
    uint32_t sliceGroupSizeAccu = (m_numRows << 8) / m_param->maxSlices;    
    uint32_t rowSum = sliceGroupSizeAccu;
    uint32_t sidx = 0;
    for (uint32_t i = 0; i < m_numRows; i++)
    {
        const uint32_t rowRange = (rowSum >> 8);
        if ((i >= rowRange) & (sidx != m_param->maxSlices - 1))
        {
            rowSum += sliceGroupSizeAccu;
            m_sliceBaseRow[++sidx] = i;
        }        
    }
    X265_CHECK(sidx < m_param->maxSlices, "sliceID check failed!");
    m_sliceBaseRow[0] = 0;
    m_sliceBaseRow[m_param->maxSlices] = m_numRows;

    m_sliceMaxBlockRow = X265_MALLOC(uint32_t, m_param->maxSlices + 1);
    ok &= !!m_sliceMaxBlockRow;
    uint32_t maxBlockRows = (m_param->sourceHeight + (16 - 1)) / 16;
    sliceGroupSizeAccu = (maxBlockRows << 8) / m_param->maxSlices;
    rowSum = sliceGroupSizeAccu;
    sidx = 0;
    for (uint32_t i = 0; i < maxBlockRows; i++)
    {
        const uint32_t rowRange = (rowSum >> 8);
        if ((i >= rowRange) & (sidx != m_param->maxSlices - 1))
        {
            rowSum += sliceGroupSizeAccu;
            m_sliceMaxBlockRow[++sidx] = i;
        }
    }
    m_sliceMaxBlockRow[0] = 0;
    m_sliceMaxBlockRow[m_param->maxSlices] = maxBlockRows;

    /* determine full motion search range */
    int range  = m_param->searchRange;       /* fpel search */
    range += !!(m_param->searchMethod < 2);  /* diamond/hex range check lag */
    range += NTAPS_LUMA / 2;                 /* subpel filter half-length */
    range += 2 + (MotionEstimate::hpelIterationCount(m_param->subpelRefine) + 1) / 2; /* subpel refine steps */
    m_refLagRows = /*(m_param->maxSlices > 1 ? 1 : 0) +*/ 1 + ((range + m_param->maxCUSize - 1) / m_param->maxCUSize);

    // NOTE: 2 times of numRows because both Encoder and Filter in same queue
    if (!WaveFront::init(m_numRows * 2))
    {
        x265_log(m_param, X265_LOG_ERROR, "unable to initialize wavefront queue\n");
        m_pool = NULL;
    }

    m_frameFilter.init(top, this, numRows, numCols);

    // initialize HRD parameters of SPS
    if (m_param->bEmitHRDSEI || !!m_param->interlaceMode)
    {
        m_rce.picTimingSEI = new SEIPictureTiming;
        m_rce.hrdTiming = new HRDTiming;

        ok &= m_rce.picTimingSEI && m_rce.hrdTiming;
    }

    if (m_param->noiseReductionIntra || m_param->noiseReductionInter || m_param->rc.vbvBufferSize)
        m_nr = X265_MALLOC(NoiseReduction, 1);
    if (m_nr)
        memset(m_nr, 0, sizeof(NoiseReduction));
    else
        m_param->noiseReductionIntra = m_param->noiseReductionInter = 0;

    // 7.4.7.1 - Ceil( Log2( PicSizeInCtbsY ) ) bits
    {
        unsigned long tmp;
        CLZ(tmp, (numRows * numCols - 1));
        m_sliceAddrBits = (uint16_t)(tmp + 1);
    }

    return ok;
}

/* Generate a complete list of unique geom sets for the current picture dimensions */
bool FrameEncoder::initializeGeoms()
{
    /* Geoms only vary between CTUs in the presence of picture edges */
    int maxCUSize = m_param->maxCUSize;
    int minCUSize = m_param->minCUSize;
    int heightRem = m_param->sourceHeight & (maxCUSize - 1);
    int widthRem = m_param->sourceWidth & (maxCUSize - 1);
    int allocGeoms = 1; // body
    if (heightRem && widthRem)
        allocGeoms = 4; // body, right, bottom, corner
    else if (heightRem || widthRem)
        allocGeoms = 2; // body, right or bottom

    m_ctuGeomMap = X265_MALLOC(uint32_t, m_numRows * m_numCols);
    m_cuGeoms = X265_MALLOC(CUGeom, allocGeoms * CUGeom::MAX_GEOMS);
    if (!m_cuGeoms || !m_ctuGeomMap)
        return false;

    // body
    CUData::calcCTUGeoms(maxCUSize, maxCUSize, maxCUSize, minCUSize, m_cuGeoms);
    memset(m_ctuGeomMap, 0, sizeof(uint32_t) * m_numRows * m_numCols);
    if (allocGeoms == 1)
        return true;

    int countGeoms = 1;
    if (widthRem)
    {
        // right
        CUData::calcCTUGeoms(widthRem, maxCUSize, maxCUSize, minCUSize, m_cuGeoms + countGeoms * CUGeom::MAX_GEOMS);
        for (uint32_t i = 0; i < m_numRows; i++)
        {
            uint32_t ctuAddr = m_numCols * (i + 1) - 1;
            m_ctuGeomMap[ctuAddr] = countGeoms * CUGeom::MAX_GEOMS;
        }
        countGeoms++;
    }
    if (heightRem)
    {
        // bottom
        CUData::calcCTUGeoms(maxCUSize, heightRem, maxCUSize, minCUSize, m_cuGeoms + countGeoms * CUGeom::MAX_GEOMS);
        for (uint32_t i = 0; i < m_numCols; i++)
        {
            uint32_t ctuAddr = m_numCols * (m_numRows - 1) + i;
            m_ctuGeomMap[ctuAddr] = countGeoms * CUGeom::MAX_GEOMS;
        }
        countGeoms++;

        if (widthRem)
        {
            // corner
            CUData::calcCTUGeoms(widthRem, heightRem, maxCUSize, minCUSize, m_cuGeoms + countGeoms * CUGeom::MAX_GEOMS);

            uint32_t ctuAddr = m_numCols * m_numRows - 1;
            m_ctuGeomMap[ctuAddr] = countGeoms * CUGeom::MAX_GEOMS;
            countGeoms++;
        }
        X265_CHECK(countGeoms == allocGeoms, "geometry match check failure\n");
    }

    return true;
}

void FrameEncoder::threadMain()
{
    THREAD_NAME("Frame", m_jpId);

    if (m_pool)
    {
        m_pool->setCurrentThreadAffinity();

        /* the first FE on each NUMA node is responsible for allocating thread
         * local data for all worker threads in that pool. If WPP is disabled, then
         * each FE also needs a TLD instance */
        if (!m_jpId)
        {
            int numTLD = m_pool->m_numWorkers;
            if (!m_param->bEnableWavefront)
                numTLD += m_pool->m_numProviders;

            m_tld = new ThreadLocalData[numTLD];
            for (int i = 0; i < numTLD; i++)
            {
                m_tld[i].analysis.initSearch(*m_param, m_top->m_scalingList);
                m_tld[i].analysis.create(m_tld);
            }

            for (int i = 0; i < m_pool->m_numProviders; i++)
            {
                if (m_pool->m_jpTable[i]->m_isFrameEncoder) /* ugh; over-allocation and other issues here */
                {
                    FrameEncoder *peer = dynamic_cast<FrameEncoder*>(m_pool->m_jpTable[i]);
                    peer->m_tld = m_tld;
                }
            }
        }

        if (m_param->bEnableWavefront)
            m_localTldIdx = -1; // cause exception if used
        else
            m_localTldIdx = m_pool->m_numWorkers + m_jpId;
    }
    else
    {
        m_tld = new ThreadLocalData;
        m_tld->analysis.initSearch(*m_param, m_top->m_scalingList);
        m_tld->analysis.create(NULL);
        m_localTldIdx = 0;
    }

    m_done.trigger();     /* signal that thread is initialized */
    m_enable.wait();      /* Encoder::encode() triggers this event */

    while (m_threadActive)
    {
        if (m_param->bCTUInfo)
        {
            while (!m_frame->m_ctuInfo)
                m_frame->m_copied.wait();
        }
        if ((m_param->bMVType == AVC_INFO) && !m_param->analysisSave && !m_param->analysisLoad && !(IS_X265_TYPE_I(m_frame->m_lowres.sliceType)))
        {
            while (((m_frame->m_analysisData.interData == NULL && m_frame->m_analysisData.intraData == NULL) || (uint32_t)m_frame->m_poc != m_frame->m_analysisData.poc))
                m_frame->m_copyMVType.wait();
        }
        m_done.trigger(); /* FrameEncoder::getEncodedPicture() blocks for this event */
        m_enable.wait();
    }
}

void FrameEncoder::WeightAnalysis::processTasks(int /* workerThreadId */)
{
    Frame* frame = master.m_frame;
    weightAnalyse(*frame->m_encData->m_slice, *frame, *master.m_param);
}


uint32_t getBsLength( int32_t code )
{
    uint32_t ucode = (code <= 0) ? -code << 1 : (code << 1) - 1;

    ++ucode;
    unsigned long idx;
    CLZ( idx, ucode );
    uint32_t length = (uint32_t)idx * 2 + 1;

    return length;
}

void FrameEncoder::encodeSlice(uint32_t sliceAddr)
{
    Slice* slice = m_frame->m_encData->m_slice;
    const uint32_t widthInLCUs = slice->m_sps->numCuInWidth;
    const uint32_t lastCUAddr = (slice->m_endCUAddr + m_param->num4x4Partitions - 1) / m_param->num4x4Partitions;
    const uint32_t numSubstreams = m_param->bEnableWavefront ? slice->m_sps->numCuInHeight : 1;

    SAOParam* saoParam = slice->m_sps->bUseSAO ? m_frame->m_encData->m_saoParam : NULL;
    for (uint32_t cuAddr = sliceAddr; cuAddr < lastCUAddr; cuAddr++)
    {
        uint32_t col = cuAddr % widthInLCUs;
        uint32_t row = cuAddr / widthInLCUs;
        uint32_t subStrm = row % numSubstreams;
        CUData* ctu = m_frame->m_encData->getPicCTU(cuAddr);

        m_entropyCoder.setBitstream(&m_outStreams[subStrm]);

        // Synchronize cabac probabilities with upper-right CTU if it's available and we're at the start of a line.
        if (m_param->bEnableWavefront && !col && row)
        {
            m_entropyCoder.copyState(m_initSliceContext);
            m_entropyCoder.loadContexts(m_rows[row - 1].bufferedEntropy);
        }

        // Initialize slice context
        if (ctu->m_bFirstRowInSlice && !col)
            m_entropyCoder.load(m_initSliceContext);

        if (saoParam)
        {
            if (saoParam->bSaoFlag[0] || saoParam->bSaoFlag[1])
            {
                int mergeLeft = col && saoParam->ctuParam[0][cuAddr].mergeMode == SAO_MERGE_LEFT;
                int mergeUp = !ctu->m_bFirstRowInSlice && saoParam->ctuParam[0][cuAddr].mergeMode == SAO_MERGE_UP;
                if (col)
                    m_entropyCoder.codeSaoMerge(mergeLeft);
                if (!ctu->m_bFirstRowInSlice && !mergeLeft)
                    m_entropyCoder.codeSaoMerge(mergeUp);
                if (!mergeLeft && !mergeUp)
                {
                    if (saoParam->bSaoFlag[0])
                        m_entropyCoder.codeSaoOffset(saoParam->ctuParam[0][cuAddr], 0);
                    if (saoParam->bSaoFlag[1])
                    {
                        m_entropyCoder.codeSaoOffset(saoParam->ctuParam[1][cuAddr], 1);
                        m_entropyCoder.codeSaoOffset(saoParam->ctuParam[2][cuAddr], 2);
                    }
                }
            }
            else
            {
                for (int i = 0; i < (m_param->internalCsp != X265_CSP_I400 ? 3 : 1); i++)
                    saoParam->ctuParam[i][cuAddr].reset();
            }
        }

        // final coding (bitstream generation) for this CU
        m_entropyCoder.encodeCTU(*ctu, m_cuGeoms[m_ctuGeomMap[cuAddr]]);

        if (m_param->bEnableWavefront)
        {
            if (col == 1)
                // Store probabilities of second CTU in line into buffer
                m_rows[row].bufferedEntropy.loadContexts(m_entropyCoder);

            if (col == widthInLCUs - 1)
                m_entropyCoder.finishSlice();
        }
    }

    if (!m_param->bEnableWavefront)
        m_entropyCoder.finishSlice();
}

void FrameEncoder::processRow(int row, int threadId)
{
    int64_t startTime = x265_mdate();
    if (ATOMIC_INC(&m_activeWorkerCount) == 1 && m_stallStartTime)
        m_totalNoWorkerTime += x265_mdate() - m_stallStartTime;

    const uint32_t realRow = m_idx_to_row[row >> 1];
    const uint32_t typeNum = m_idx_to_row[row & 1];

    if (!typeNum)
        processRowEncoder(realRow, m_tld[threadId]);
    else
    {
        m_frameFilter.processRow(realRow);

        // NOTE: Active next row
        if (realRow != m_sliceBaseRow[m_rows[realRow].sliceId + 1] - 1)
            enqueueRowFilter(m_row_to_idx[realRow + 1]);
    }

    if (ATOMIC_DEC(&m_activeWorkerCount) == 0)
        m_stallStartTime = x265_mdate();

    m_totalWorkerElapsedTime += x265_mdate() - startTime; // not thread safe, but good enough
}

// Called by worker threads
void FrameEncoder::processRowEncoder(int intRow, ThreadLocalData& tld)
{
    const uint32_t row = (uint32_t)intRow;
    CTURow& curRow = m_rows[row];

    if (m_param->bEnableWavefront)
    {
        ScopedLock self(curRow.lock);
        if (!curRow.active)
            /* VBV restart is in progress, exit out */
            return;
        if (curRow.busy)
        {
            /* On multi-socket Windows servers, we have seen problems with
             * ATOMIC_CAS which resulted in multiple worker threads processing
             * the same CU row, which often resulted in bad pointer accesses. We
             * believe the problem is fixed, but are leaving this check in place
             * to prevent crashes in case it is not */
            x265_log(m_param, X265_LOG_WARNING,
                     "internal error - simultaneous row access detected. Please report HW to x265-devel@videolan.org\n");
            return;
        }
        curRow.busy = true;
    }

    /* When WPP is enabled, every row has its own row coder instance. Otherwise
     * they share row 0 */
    Entropy& rowCoder = m_param->bEnableWavefront ? curRow.rowGoOnCoder : m_rows[0].rowGoOnCoder;
    FrameData& curEncData = *m_frame->m_encData;
    Slice *slice = curEncData.m_slice;

    const uint32_t numCols = m_numCols;
    const uint32_t lineStartCUAddr = row * numCols;
    bool bIsVbv = m_param->rc.vbvBufferSize > 0 && m_param->rc.vbvMaxBitrate > 0;

    const uint32_t sliceId = curRow.sliceId;
    uint32_t maxBlockCols = (m_frame->m_fencPic->m_picWidth + (16 - 1)) / 16;
    uint32_t noOfBlocks = m_param->maxCUSize / 16;
    const uint32_t bFirstRowInSlice = ((row == 0) || (m_rows[row - 1].sliceId != curRow.sliceId)) ? 1 : 0;
    const uint32_t bLastRowInSlice = ((row == m_numRows - 1) || (m_rows[row + 1].sliceId != curRow.sliceId)) ? 1 : 0;
    const uint32_t endRowInSlicePlus1 = m_sliceBaseRow[sliceId + 1];
    const uint32_t rowInSlice = row - m_sliceBaseRow[sliceId];

    // Load SBAC coder context from previous row and initialize row state.
    if (bFirstRowInSlice && !curRow.completed)        
        rowCoder.load(m_initSliceContext);     

    // calculate mean QP for consistent deltaQP signalling calculation
    if (m_param->bOptCUDeltaQP)
    {
        ScopedLock self(curRow.lock);
        if (!curRow.avgQPComputed)
        {
            if (m_param->bEnableWavefront || !row)
            {
                double meanQPOff = 0;
                bool isReferenced = IS_REFERENCED(m_frame);
                double *qpoffs = (isReferenced && m_param->rc.cuTree) ? m_frame->m_lowres.qpCuTreeOffset : m_frame->m_lowres.qpAqOffset;
                if (qpoffs)
                {
                    uint32_t loopIncr = (m_param->rc.qgSize == 8) ? 8 : 16;

                    uint32_t cuYStart = 0, height = m_frame->m_fencPic->m_picHeight;
                    if (m_param->bEnableWavefront)
                    {
                        cuYStart = intRow * m_param->maxCUSize;
                        height = cuYStart + m_param->maxCUSize;
                    }

                    uint32_t qgSize = m_param->rc.qgSize, width = m_frame->m_fencPic->m_picWidth;
                    uint32_t maxOffsetCols = (m_frame->m_fencPic->m_picWidth + (loopIncr - 1)) / loopIncr;
                    uint32_t count = 0;
                    for (uint32_t cuY = cuYStart; cuY < height && (cuY < m_frame->m_fencPic->m_picHeight); cuY += qgSize)
                    {
                        for (uint32_t cuX = 0; cuX < width; cuX += qgSize)
                        {
                            double qp_offset = 0;
                            uint32_t cnt = 0;

                            for (uint32_t block_yy = cuY; block_yy < cuY + qgSize && block_yy < m_frame->m_fencPic->m_picHeight; block_yy += loopIncr)
                            {
                                for (uint32_t block_xx = cuX; block_xx < cuX + qgSize && block_xx < width; block_xx += loopIncr)
                                {
                                    int idx = ((block_yy / loopIncr) * (maxOffsetCols)) + (block_xx / loopIncr);
                                    qp_offset += qpoffs[idx];
                                    cnt++;
                                }
                            }
                            qp_offset /= cnt;
                            meanQPOff += qp_offset;
                            count++;
                        }
                    }
                    meanQPOff /= count;
                }
                rowCoder.m_meanQP = slice->m_sliceQp + meanQPOff;
            }
            else
            {
                rowCoder.m_meanQP = m_rows[0].rowGoOnCoder.m_meanQP;
            }
            curRow.avgQPComputed = 1;
        }
    }    

    // Initialize restrict on MV range in slices
    tld.analysis.m_sliceMinY = -(int16_t)(rowInSlice * m_param->maxCUSize * 4) + 3 * 4;
    tld.analysis.m_sliceMaxY = (int16_t)((endRowInSlicePlus1 - 1 - row) * (m_param->maxCUSize * 4) - 4 * 4);

    // Handle single row slice
    if (tld.analysis.m_sliceMaxY < tld.analysis.m_sliceMinY)
        tld.analysis.m_sliceMaxY = tld.analysis.m_sliceMinY = 0;


    while (curRow.completed < numCols)
    {
        ProfileScopeEvent(encodeCTU);

        const uint32_t col = curRow.completed;
        const uint32_t cuAddr = lineStartCUAddr + col;
        CUData* ctu = curEncData.getPicCTU(cuAddr);
        const uint32_t bLastCuInSlice = (bLastRowInSlice & (col == numCols - 1)) ? 1 : 0;
        ctu->initCTU(*m_frame, cuAddr, slice->m_sliceQp, bFirstRowInSlice, bLastRowInSlice, bLastCuInSlice);

        if (bIsVbv)
        {
            if (col == 0 && !m_param->bEnableWavefront)
            {
                m_backupStreams[0].copyBits(&m_outStreams[0]);
                curRow.bufferedEntropy.copyState(rowCoder);
                curRow.bufferedEntropy.loadContexts(rowCoder);
            }
            if (bFirstRowInSlice && m_vbvResetTriggerRow != intRow)            
            {
                curEncData.m_rowStat[row].rowQp = curEncData.m_avgQpRc;
                curEncData.m_rowStat[row].rowQpScale = x265_qp2qScale(curEncData.m_avgQpRc);
            }

            FrameData::RCStatCU& cuStat = curEncData.m_cuStat[cuAddr];
            if (m_param->bEnableWavefront && rowInSlice >= col && !bFirstRowInSlice && m_vbvResetTriggerRow != intRow)
                cuStat.baseQp = curEncData.m_cuStat[cuAddr - numCols + 1].baseQp;
            else if (!m_param->bEnableWavefront && !bFirstRowInSlice && m_vbvResetTriggerRow != intRow)
                cuStat.baseQp = curEncData.m_rowStat[row - 1].rowQp;
            else
                cuStat.baseQp = curEncData.m_rowStat[row].rowQp;

            /* TODO: use defines from slicetype.h for lowres block size */
            uint32_t block_y = (ctu->m_cuPelY >> m_param->maxLog2CUSize) * noOfBlocks;
            uint32_t block_x = (ctu->m_cuPelX >> m_param->maxLog2CUSize) * noOfBlocks;
            if (!m_param->analysisLoad || !m_param->bDisableLookahead)
            {
                cuStat.vbvCost = 0;
                cuStat.intraVbvCost = 0;

                for (uint32_t h = 0; h < noOfBlocks && block_y < m_sliceMaxBlockRow[sliceId + 1]; h++, block_y++)
                {
                    uint32_t idx = block_x + (block_y * maxBlockCols);

                    for (uint32_t w = 0; w < noOfBlocks && (block_x + w) < maxBlockCols; w++, idx++)
                    {
                        cuStat.vbvCost += m_frame->m_lowres.lowresCostForRc[idx] & LOWRES_COST_MASK;
                        cuStat.intraVbvCost += m_frame->m_lowres.intraCost[idx];
                    }
                }
            }
        }
        else
            curEncData.m_cuStat[cuAddr].baseQp = curEncData.m_avgQpRc;

        if (m_param->bEnableWavefront && !col && !bFirstRowInSlice)
        {
            // Load SBAC coder context from previous row and initialize row state.
            rowCoder.copyState(m_initSliceContext);
            rowCoder.loadContexts(m_rows[row - 1].bufferedEntropy);
        }
        if (m_param->dynamicRd && (int32_t)(m_rce.qpaRc - m_rce.qpNoVbv) > 0)
            ctu->m_vbvAffected = true;

        // Does all the CU analysis, returns best top level mode decision
        Mode& best = tld.analysis.compressCTU(*ctu, *m_frame, m_cuGeoms[m_ctuGeomMap[cuAddr]], rowCoder);
        if (m_param->bDynamicRefine)
        {
            if (m_top->m_startPoint <= m_frame->m_encodeOrder) // Avoid collecting data that will not be used by future frames.
            {
                ScopedLock dynLock(m_top->m_dynamicRefineLock);
                for (uint32_t i = 0; i < X265_REFINE_INTER_LEVELS; i++)
                {
                    for (uint32_t depth = 0; depth < m_param->maxCUDepth; depth++)
                    {
                        int offset = (depth * X265_REFINE_INTER_LEVELS) + i;
                        int curFrameIndex = m_frame->m_encodeOrder - m_top->m_startPoint;
                        int index = (curFrameIndex * X265_REFINE_INTER_LEVELS * m_param->maxCUDepth) + offset;
                        if (ctu->m_collectCUCount[offset])
                        {
                            m_top->m_variance[index] += ctu->m_collectCUVariance[offset];
                            m_top->m_rdCost[index] += ctu->m_collectCURd[offset];
                            m_top->m_trainingCount[index] += ctu->m_collectCUCount[offset];
                        }
                    }
                }
            }
            X265_FREE_ZERO(ctu->m_collectCUVariance);
            X265_FREE_ZERO(ctu->m_collectCURd);
            X265_FREE_ZERO(ctu->m_collectCUCount);
        }

        // take a sample of the current active worker count
        ATOMIC_ADD(&m_totalActiveWorkerCount, m_activeWorkerCount);
        ATOMIC_INC(&m_activeWorkerCountSamples);

        /* advance top-level row coder to include the context of this CTU.
         * if SAO is disabled, rowCoder writes the final CTU bitstream */
        rowCoder.encodeCTU(*ctu, m_cuGeoms[m_ctuGeomMap[cuAddr]]);

        if (m_param->bEnableWavefront && col == 1)
            // Save CABAC state for next row
            curRow.bufferedEntropy.loadContexts(rowCoder);

        /* SAO parameter estimation using non-deblocked pixels for CTU bottom and right boundary areas */
        if (m_param->bEnableSAO && m_param->bSaoNonDeblocked)
            m_frameFilter.m_parallelFilter[row].m_sao.calcSaoStatsCu_BeforeDblk(m_frame, col, row);

        /* Deblock with idle threading */
        if (m_param->bEnableLoopFilter | m_param->bEnableSAO)
        {
            // NOTE: in VBV mode, we may reencode anytime, so we can't do Deblock stage-Horizon and SAO
            if (!bIsVbv)
            {                
                // Delay one row to avoid intra prediction conflict
                if (m_pool && !bFirstRowInSlice)
                {                    
                    int allowCol = col;

                    // avoid race condition on last column
                    if (rowInSlice >= 2)
                    {
                        allowCol = X265_MIN(((col == numCols - 1) ? m_frameFilter.m_parallelFilter[row - 2].m_lastDeblocked.get()
                                                                  : m_frameFilter.m_parallelFilter[row - 2].m_lastCol.get()), (int)col);
                    }
                    m_frameFilter.m_parallelFilter[row - 1].m_allowedCol.set(allowCol);
                }

                // Last Row may start early
                if (m_pool && bLastRowInSlice)
                {
                    // Deblocking last row
                    int allowCol = col;

                    // avoid race condition on last column
                    if (rowInSlice >= 2)
                    {
                        allowCol = X265_MIN(((col == numCols - 1) ? m_frameFilter.m_parallelFilter[row - 1].m_lastDeblocked.get()
                                                                  : m_frameFilter.m_parallelFilter[row - 1].m_lastCol.get()), (int)col);
                    }
                    m_frameFilter.m_parallelFilter[row].m_allowedCol.set(allowCol);
                }
            } // end of !bIsVbv
        }
        // Both Loopfilter and SAO Disabled
        else
        {
            m_frameFilter.m_parallelFilter[row].processPostCu(col);
        }

        // Completed CU processing
        curRow.completed++;

        FrameStats frameLog;
        curEncData.m_rowStat[row].sumQpAq += collectCTUStatistics(*ctu, &frameLog);

        // copy number of intra, inter cu per row into frame stats for 2 pass
        if (m_param->rc.bStatWrite)
        {
            curRow.rowStats.mvBits    += best.mvBits;
            curRow.rowStats.coeffBits += best.coeffBits;
            curRow.rowStats.miscBits  += best.totalBits - (best.mvBits + best.coeffBits);

            for (uint32_t depth = 0; depth <= m_param->maxCUDepth; depth++)
            {
                /* 1 << shift == number of 8x8 blocks at current depth */
                int shift = 2 * (m_param->maxCUDepth - depth);
                int cuSize = m_param->maxCUSize >> depth;

                curRow.rowStats.intra8x8Cnt += (cuSize == 8) ? (int)(frameLog.cntIntra[depth] + frameLog.cntIntraNxN) :
                                                               (int)(frameLog.cntIntra[depth] << shift);

                curRow.rowStats.inter8x8Cnt += (int)(frameLog.cntInter[depth] << shift);
                curRow.rowStats.skip8x8Cnt += (int)((frameLog.cntSkipCu[depth] + frameLog.cntMergeCu[depth]) << shift);
            }
        }
        curRow.rowStats.totalCtu++;
        curRow.rowStats.lumaDistortion   += best.lumaDistortion;
        curRow.rowStats.chromaDistortion += best.chromaDistortion;
        curRow.rowStats.psyEnergy        += best.psyEnergy;
        curRow.rowStats.ssimEnergy       += best.ssimEnergy;
        curRow.rowStats.resEnergy        += best.resEnergy;
        curRow.rowStats.cntIntraNxN      += frameLog.cntIntraNxN;
        curRow.rowStats.totalCu          += frameLog.totalCu;
        for (uint32_t depth = 0; depth <= m_param->maxCUDepth; depth++)
        {
            curRow.rowStats.cntSkipCu[depth] += frameLog.cntSkipCu[depth];
            curRow.rowStats.cntMergeCu[depth] += frameLog.cntMergeCu[depth];
            for (int m = 0; m < INTER_MODES; m++)
                curRow.rowStats.cuInterDistribution[depth][m] += frameLog.cuInterDistribution[depth][m];
            for (int n = 0; n < INTRA_MODES; n++)
                curRow.rowStats.cuIntraDistribution[depth][n] += frameLog.cuIntraDistribution[depth][n];
        }

        curEncData.m_cuStat[cuAddr].totalBits = best.totalBits;
        x265_emms();

        if (bIsVbv)
        {   
            // Update encoded bits, satdCost, baseQP for each CU if tune grain is disabled
            FrameData::RCStatCU& cuStat = curEncData.m_cuStat[cuAddr];    
            if ((m_param->bEnableWavefront && ((cuAddr == m_sliceBaseRow[sliceId] * numCols) || !m_param->rc.bEnableConstVbv)) || !m_param->bEnableWavefront)
            {
                curEncData.m_rowStat[row].rowSatd += cuStat.vbvCost;
                curEncData.m_rowStat[row].rowIntraSatd += cuStat.intraVbvCost;
                curEncData.m_rowStat[row].encodedBits += cuStat.totalBits;
                curEncData.m_rowStat[row].sumQpRc += cuStat.baseQp;
                curEncData.m_rowStat[row].numEncodedCUs = cuAddr;
            }
            
            // If current block is at row end checkpoint, call vbv ratecontrol.
            if (!m_param->bEnableWavefront && col == numCols - 1)
            {
                double qpBase = curEncData.m_cuStat[cuAddr].baseQp;
                int reEncode = m_top->m_rateControl->rowVbvRateControl(m_frame, row, &m_rce, qpBase, m_sliceBaseRow, sliceId);
                qpBase = x265_clip3((double)m_param->rc.qpMin, (double)m_param->rc.qpMax, qpBase);
                curEncData.m_rowStat[row].rowQp = qpBase;
                curEncData.m_rowStat[row].rowQpScale = x265_qp2qScale(qpBase);
                if (reEncode < 0)
                {
                    x265_log(m_param, X265_LOG_DEBUG, "POC %d row %d - encode restart required for VBV, to %.2f from %.2f\n",
                        m_frame->m_poc, row, qpBase, curEncData.m_cuStat[cuAddr].baseQp);

                    m_vbvResetTriggerRow = row;
                    m_outStreams[0].copyBits(&m_backupStreams[0]);

                    rowCoder.copyState(curRow.bufferedEntropy);
                    rowCoder.loadContexts(curRow.bufferedEntropy);

                    curRow.completed = 0;
                    memset(&curRow.rowStats, 0, sizeof(curRow.rowStats));
                    curEncData.m_rowStat[row].numEncodedCUs = 0;
                    curEncData.m_rowStat[row].encodedBits = 0;
                    curEncData.m_rowStat[row].rowSatd = 0;
                    curEncData.m_rowStat[row].rowIntraSatd = 0;
                    curEncData.m_rowStat[row].sumQpRc = 0;
                    curEncData.m_rowStat[row].sumQpAq = 0;
                }
            }
            // If current block is at row diagonal checkpoint, call vbv ratecontrol.
            else if (m_param->bEnableWavefront && rowInSlice == col && !bFirstRowInSlice)
            {
                if (m_param->rc.bEnableConstVbv)
                {
                    uint32_t startCuAddr = numCols * row;
                    uint32_t EndCuAddr = startCuAddr + col;

                    for (int32_t r = row; r >= (int32_t)m_sliceBaseRow[sliceId]; r--)
                    {
                        for (uint32_t c = startCuAddr; c <= EndCuAddr && c <= numCols * (r + 1) - 1; c++)
                        {
                            curEncData.m_rowStat[r].rowSatd += curEncData.m_cuStat[c].vbvCost;
                            curEncData.m_rowStat[r].rowIntraSatd += curEncData.m_cuStat[c].intraVbvCost;
                            curEncData.m_rowStat[r].encodedBits += curEncData.m_cuStat[c].totalBits;
                            curEncData.m_rowStat[r].sumQpRc += curEncData.m_cuStat[c].baseQp;
                            curEncData.m_rowStat[r].numEncodedCUs = c;
                        }
                        startCuAddr = EndCuAddr - numCols;
                        EndCuAddr = startCuAddr + 1;
                    }
                }
                double qpBase = curEncData.m_cuStat[cuAddr].baseQp;
                int reEncode = m_top->m_rateControl->rowVbvRateControl(m_frame, row, &m_rce, qpBase, m_sliceBaseRow, sliceId);
                qpBase = x265_clip3((double)m_param->rc.qpMin, (double)m_param->rc.qpMax, qpBase);
                curEncData.m_rowStat[row].rowQp = qpBase;
                curEncData.m_rowStat[row].rowQpScale = x265_qp2qScale(qpBase);

                if (reEncode < 0)
                {
                    x265_log(m_param, X265_LOG_DEBUG, "POC %d row %d - encode restart required for VBV, to %.2f from %.2f\n",
                             m_frame->m_poc, row, qpBase, curEncData.m_cuStat[cuAddr].baseQp);

                    // prevent the WaveFront::findJob() method from providing new jobs
                    m_vbvResetTriggerRow = row;
                    m_bAllRowsStop = true;

                    for (uint32_t r = m_sliceBaseRow[sliceId + 1] - 1; r >= row; r--)
                    {
                        CTURow& stopRow = m_rows[r];

                        if (r != row)
                        {
                            /* if row was active (ready to be run) clear active bit and bitmap bit for this row */
                            stopRow.lock.acquire();
                            while (stopRow.active)
                            {
                                if (dequeueRow(r * 2))
                                    stopRow.active = false;
                                else
                                {
                                    /* we must release the row lock to allow the thread to exit */
                                    stopRow.lock.release();
                                    GIVE_UP_TIME();
                                    stopRow.lock.acquire();
                                }
                            }
                            stopRow.lock.release();

                            bool bRowBusy = true;
                            do
                            {
                                stopRow.lock.acquire();
                                bRowBusy = stopRow.busy;
                                stopRow.lock.release();

                                if (bRowBusy)
                                {
                                    GIVE_UP_TIME();
                                }
                            }
                            while (bRowBusy);
                        }

                        m_outStreams[r].resetBits();
                        stopRow.completed = 0;
                        memset(&stopRow.rowStats, 0, sizeof(stopRow.rowStats));
                        curEncData.m_rowStat[r].numEncodedCUs = 0;
                        curEncData.m_rowStat[r].encodedBits = 0;
                        curEncData.m_rowStat[r].rowSatd = 0;
                        curEncData.m_rowStat[r].rowIntraSatd = 0;
                        curEncData.m_rowStat[r].sumQpRc = 0;
                        curEncData.m_rowStat[r].sumQpAq = 0;
                    }

                    m_bAllRowsStop = false;
                }
            }
        }

        if (m_param->bEnableWavefront && curRow.completed >= 2 && !bLastRowInSlice &&
            (!m_bAllRowsStop || intRow + 1 < m_vbvResetTriggerRow))
        {
            /* activate next row */
            ScopedLock below(m_rows[row + 1].lock);

            if (m_rows[row + 1].active == false &&
                m_rows[row + 1].completed + 2 <= curRow.completed)
            {
                m_rows[row + 1].active = true;
                enqueueRowEncoder(m_row_to_idx[row + 1]);
                tryWakeOne(); /* wake up a sleeping thread or set the help wanted flag */
            }
        }

        ScopedLock self(curRow.lock);
        if ((m_bAllRowsStop && intRow > m_vbvResetTriggerRow) ||
            (!bFirstRowInSlice && ((curRow.completed < numCols - 1) || (m_rows[row - 1].completed < numCols)) && m_rows[row - 1].completed < curRow.completed + 2))
        {
            curRow.active = false;
            curRow.busy = false;
            ATOMIC_INC(&m_countRowBlocks);
            return;
        }
    }

    /* this row of CTUs has been compressed */
    if (m_param->bEnableWavefront && m_param->rc.bEnableConstVbv)
    {
        if (bLastRowInSlice)       
        {
            for (uint32_t r = m_sliceBaseRow[sliceId]; r < m_sliceBaseRow[sliceId + 1]; r++)
            {
                for (uint32_t c = curEncData.m_rowStat[r].numEncodedCUs + 1; c < numCols * (r + 1); c++)
                {
                    curEncData.m_rowStat[r].rowSatd += curEncData.m_cuStat[c].vbvCost;
                    curEncData.m_rowStat[r].rowIntraSatd += curEncData.m_cuStat[c].intraVbvCost;
                    curEncData.m_rowStat[r].encodedBits += curEncData.m_cuStat[c].totalBits;
                    curEncData.m_rowStat[r].sumQpRc += curEncData.m_cuStat[c].baseQp;
                    curEncData.m_rowStat[r].numEncodedCUs = c;
                }
            }
        }
    }

    /* If encoding with ABR, update update bits and complexity in rate control
     * after a number of rows so the next frame's rateControlStart has more
     * accurate data for estimation. At the start of the encode we update stats
     * after half the frame is encoded, but after this initial period we update
     * after refLagRows (the number of rows reference frames must have completed
     * before referencees may begin encoding) */
    if (m_param->rc.rateControlMode == X265_RC_ABR || bIsVbv)
    {
        uint32_t rowCount = 0;
        uint32_t maxRows = m_sliceBaseRow[sliceId + 1] - m_sliceBaseRow[sliceId];

        if (!m_rce.encodeOrder)
            rowCount = maxRows - 1; 
        else if ((uint32_t)m_rce.encodeOrder <= 2 * (m_param->fpsNum / m_param->fpsDenom))
            rowCount = X265_MIN((maxRows + 1) / 2, maxRows - 1);
        else
			rowCount = X265_MIN(m_refLagRows / m_param->maxSlices, maxRows - 1);

        if (rowInSlice == rowCount)
        {
            m_rowSliceTotalBits[sliceId] = 0;
            if (bIsVbv && !(m_param->rc.bEnableConstVbv && m_param->bEnableWavefront))
            {          
                for (uint32_t i = m_sliceBaseRow[sliceId]; i < rowCount + m_sliceBaseRow[sliceId]; i++)
                    m_rowSliceTotalBits[sliceId] += curEncData.m_rowStat[i].encodedBits;
            }
            else
            {
                uint32_t startAddr = m_sliceBaseRow[sliceId] * numCols;
				uint32_t finishAddr = startAddr + rowCount * numCols;
                
				for (uint32_t cuAddr = startAddr; cuAddr < finishAddr; cuAddr++)
                    m_rowSliceTotalBits[sliceId] += curEncData.m_cuStat[cuAddr].totalBits;
            }            

            if (ATOMIC_INC(&m_sliceCnt) == (int)m_param->maxSlices)
            {
                m_rce.rowTotalBits = 0;
                for (uint32_t i = 0; i < m_param->maxSlices; i++)
                    m_rce.rowTotalBits += m_rowSliceTotalBits[i];
                m_top->m_rateControl->rateControlUpdateStats(&m_rce);
            }
        }
    }

    /* flush row bitstream (if WPP and no SAO) or flush frame if no WPP and no SAO */
    /* end_of_sub_stream_one_bit / end_of_slice_segment_flag */
    if (!m_param->bEnableSAO && (m_param->bEnableWavefront || bLastRowInSlice))
        rowCoder.finishSlice();


    /* Processing left Deblock block with current threading */
    if ((m_param->bEnableLoopFilter | m_param->bEnableSAO) & (rowInSlice >= 2))
    {
        /* Check conditional to start previous row process with current threading */
        if (m_frameFilter.m_parallelFilter[row - 2].m_lastDeblocked.get() == (int)numCols)
        {
            /* stop threading on current row and restart it */
            m_frameFilter.m_parallelFilter[row - 1].m_allowedCol.set(numCols);
            m_frameFilter.m_parallelFilter[row - 1].processTasks(-1);
        }
    }

    /* trigger row-wise loop filters */
    if (m_param->bEnableWavefront)
    {
        if (rowInSlice >= m_filterRowDelay)
        {
            enableRowFilter(m_row_to_idx[row - m_filterRowDelay]);

            /* NOTE: Activate filter if first row (row 0) */
            if (rowInSlice == m_filterRowDelay)
                enqueueRowFilter(m_row_to_idx[row - m_filterRowDelay]);
            tryWakeOne();
        }

        if (bLastRowInSlice)
        {
            for (uint32_t i = endRowInSlicePlus1 - m_filterRowDelay; i < endRowInSlicePlus1; i++)
            {
                enableRowFilter(m_row_to_idx[i]);
            }
            tryWakeOne();
        }

        // handle specially case - single row slice
        if  (bFirstRowInSlice & bLastRowInSlice)
        {
            enqueueRowFilter(m_row_to_idx[row]);
            tryWakeOne();
        }
    }

    curRow.busy = false;

    // CHECK_ME: Does it always FALSE condition?
    if (ATOMIC_INC(&m_completionCount) == 2 * (int)m_numRows)
        m_completionEvent.trigger();
}
void FrameEncoder::computeAvgTrainingData()
{
    if (m_frame->m_lowres.bScenecut || m_frame->m_lowres.bKeyframe)
    {
        m_top->m_startPoint = m_frame->m_encodeOrder;
        int size = (m_param->keyframeMax + m_param->lookaheadDepth) * m_param->maxCUDepth * X265_REFINE_INTER_LEVELS;
        memset(m_top->m_variance, 0, size * sizeof(uint64_t));
        memset(m_top->m_rdCost, 0, size * sizeof(uint64_t));
        memset(m_top->m_trainingCount, 0, size * sizeof(uint32_t));
    }
    if (m_frame->m_encodeOrder - m_top->m_startPoint < 2 * m_param->frameNumThreads)
        m_frame->m_classifyFrame = false;
    else
        m_frame->m_classifyFrame = true;

    int size = m_param->maxCUDepth * X265_REFINE_INTER_LEVELS;
    memset(m_frame->m_classifyRd, 0, size * sizeof(uint64_t));
    memset(m_frame->m_classifyVariance, 0, size * sizeof(uint64_t));
    memset(m_frame->m_classifyCount, 0, size * sizeof(uint32_t));
    if (m_frame->m_classifyFrame)
    {
        uint32_t limit = m_frame->m_encodeOrder - m_top->m_startPoint - m_param->frameNumThreads;
        for (uint32_t i = 1; i < limit; i++)
        {
            for (uint32_t j = 0; j < X265_REFINE_INTER_LEVELS; j++)
            {
                for (uint32_t depth = 0; depth < m_param->maxCUDepth; depth++)
                {
                    int offset = (depth * X265_REFINE_INTER_LEVELS) + j;
                    int index = (i* X265_REFINE_INTER_LEVELS * m_param->maxCUDepth) + offset;
                    if (m_top->m_trainingCount[index])
                    {
                        m_frame->m_classifyRd[offset] += m_top->m_rdCost[index] / m_top->m_trainingCount[index];
                        m_frame->m_classifyVariance[offset] += m_top->m_variance[index] / m_top->m_trainingCount[index];
                        m_frame->m_classifyCount[offset] += m_top->m_trainingCount[index];
                    }
                }
            }
        }
        /* Calculates the average feature values of historic frames that are being considered for the current frame */
        int historyCount = m_frame->m_encodeOrder - m_param->frameNumThreads - m_top->m_startPoint - 1;
        if (historyCount)
        {
            for (uint32_t j = 0; j < X265_REFINE_INTER_LEVELS; j++)
            {
                for (uint32_t depth = 0; depth < m_param->maxCUDepth; depth++)
                {
                    int offset = (depth * X265_REFINE_INTER_LEVELS) + j;
                    m_frame->m_classifyRd[offset] /= historyCount;
                    m_frame->m_classifyVariance[offset] /= historyCount;
                }
            }
        }
    }
}

/* collect statistics about CU coding decisions, return total QP */
int FrameEncoder::collectCTUStatistics(const CUData& ctu, FrameStats* log)
{
    int totQP = 0;
    uint32_t depth = 0;
    for (uint32_t absPartIdx = 0; absPartIdx < ctu.m_numPartitions; absPartIdx += ctu.m_numPartitions >> (depth * 2))
    {
        depth = ctu.m_cuDepth[absPartIdx];
        totQP += ctu.m_qp[absPartIdx] * (ctu.m_numPartitions >> (depth * 2));
    }

    if (m_param->csvLogLevel >= 1 || m_param->rc.bStatWrite)
    {
        if (ctu.m_slice->m_sliceType == I_SLICE)
        {
            depth = 0;
            for (uint32_t absPartIdx = 0; absPartIdx < ctu.m_numPartitions; absPartIdx += ctu.m_numPartitions >> (depth * 2))
            {
                depth = ctu.m_cuDepth[absPartIdx];

                log->totalCu++;
                log->cntIntra[depth]++;

                if (ctu.m_predMode[absPartIdx] == MODE_NONE)
                {
                    log->totalCu--;
                    log->cntIntra[depth]--;
                }
                else if (ctu.m_partSize[absPartIdx] != SIZE_2Nx2N)
                {
                    /* TODO: log intra modes at absPartIdx +0 to +3 */
                    X265_CHECK(ctu.m_log2CUSize[absPartIdx] == 3 && ctu.m_slice->m_sps->quadtreeTULog2MinSize < 3, "Intra NxN found at improbable depth\n");
                    log->cntIntraNxN++;
                    log->cntIntra[depth]--;
                }
                else if (ctu.m_lumaIntraDir[absPartIdx] > 1)
                    log->cuIntraDistribution[depth][ANGULAR_MODE_ID]++;
                else
                    log->cuIntraDistribution[depth][ctu.m_lumaIntraDir[absPartIdx]]++;
            }
        }
        else
        {
            depth = 0;
            for (uint32_t absPartIdx = 0; absPartIdx < ctu.m_numPartitions; absPartIdx += ctu.m_numPartitions >> (depth * 2))
            {
                depth = ctu.m_cuDepth[absPartIdx];

                log->totalCu++;

                if (ctu.m_predMode[absPartIdx] == MODE_NONE)
                    log->totalCu--;
                else if (ctu.isSkipped(absPartIdx))
                {
                    if (ctu.m_mergeFlag[0])
                        log->cntMergeCu[depth]++;
                    else
                        log->cntSkipCu[depth]++;
                }
                else if (ctu.isInter(absPartIdx))
                {
                    log->cntInter[depth]++;

                    if (ctu.m_partSize[absPartIdx] < AMP_ID)
                        log->cuInterDistribution[depth][ctu.m_partSize[absPartIdx]]++;
                    else
                        log->cuInterDistribution[depth][AMP_ID]++;
                }
                else if (ctu.isIntra(absPartIdx))
                {
                    log->cntIntra[depth]++;

                    if (ctu.m_partSize[absPartIdx] != SIZE_2Nx2N)
                    {
                        X265_CHECK(ctu.m_log2CUSize[absPartIdx] == 3 && ctu.m_slice->m_sps->quadtreeTULog2MinSize < 3, "Intra NxN found at improbable depth\n");
                        log->cntIntraNxN++;
                        log->cntIntra[depth]--;
                        /* TODO: log intra modes at absPartIdx +0 to +3 */
                    }
                    else if (ctu.m_lumaIntraDir[absPartIdx] > 1)
                        log->cuIntraDistribution[depth][ANGULAR_MODE_ID]++;
                    else
                        log->cuIntraDistribution[depth][ctu.m_lumaIntraDir[absPartIdx]]++;
                }
            }
        }
    }

    return totQP;
}

/* DCT-domain noise reduction / adaptive deadzone from libavcodec */
void FrameEncoder::noiseReductionUpdate()
{
    static const uint32_t maxBlocksPerTrSize[4] = {1 << 18, 1 << 16, 1 << 14, 1 << 12};

    for (int cat = 0; cat < MAX_NUM_TR_CATEGORIES; cat++)
    {
        int trSize = cat & 3;
        int coefCount = 1 << ((trSize + 2) * 2);

        if (m_nr->nrCount[cat] > maxBlocksPerTrSize[trSize])
        {
            for (int i = 0; i < coefCount; i++)
                m_nr->nrResidualSum[cat][i] >>= 1;
            m_nr->nrCount[cat] >>= 1;
        }

        int nrStrength = cat < 8 ? m_param->noiseReductionIntra : m_param->noiseReductionInter;
        uint64_t scaledCount = (uint64_t)nrStrength * m_nr->nrCount[cat];

        for (int i = 0; i < coefCount; i++)
        {
            uint64_t value = scaledCount + m_nr->nrResidualSum[cat][i] / 2;
            uint64_t denom = m_nr->nrResidualSum[cat][i] + 1;
            m_nr->nrOffsetDenoise[cat][i] = (uint16_t)(value / denom);
        }

        // Don't denoise DC coefficients
        m_nr->nrOffsetDenoise[cat][0] = 0;
    }
}
#if ENABLE_LIBVMAF
void FrameEncoder::vmafFrameLevelScore()
{
    PicYuv *fenc = m_frame->m_fencPic;
    PicYuv *recon = m_frame->m_reconPic;

    x265_vmaf_framedata *vmafframedata = (x265_vmaf_framedata*)x265_malloc(sizeof(x265_vmaf_framedata));
    if (!vmafframedata)
    {
        x265_log(NULL, X265_LOG_ERROR, "vmaf frame data alloc failed\n");
    }

    vmafframedata->height = fenc->m_picHeight;
    vmafframedata->width = fenc->m_picWidth;
    vmafframedata->frame_set = 0;
    vmafframedata->internalBitDepth = m_param->internalBitDepth;
    vmafframedata->reference_frame = fenc;
    vmafframedata->distorted_frame = recon;

    fenc->m_vmafScore = x265_calculate_vmaf_framelevelscore(vmafframedata);

    if (vmafframedata)
    x265_free(vmafframedata);
}
#endif

}
