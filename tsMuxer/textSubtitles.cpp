#include "textSubtitles.h"

#ifdef _WIN32
#include <winerror.h>

#include "osdep/textSubtitlesRenderWin32.h"
#ifdef WIN32_DEBUG_FREETYPE
#include "osdep/textSubtitlesRenderFT.h"
#endif
#else
#include "osdep/textSubtitlesRenderFT.h"
#endif

#include <assert.h>

#include "math.h"
#include "memory.h"
#include "psgStreamReader.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;
using namespace text_subtitles;

static const float FLOAT_EPS = 1e-6f;
static const int PG_BUFFER_SIZE = 1024 * 1024 * 2;

// const static int BORDER_WIDTH = 4;
// const RGBQUAD RESERVED_BACKGROUND_COLOR = {0x00, 0x00, 0x00, 0x00};

///////////////////////////////////////////
TextToPGSConverter::TextToPGSConverter(bool sourceIsText)
    : /* TextSubtitlesRenderWin32(), */
      m_rleLen(0),
      m_composition_number(0),
      m_videoWidth(0),
      m_videoHeight(0),
      m_videoFps(0),
      m_imageBuffer()
{
    m_bottomOffset = 0;
    m_renderedData = 0;
    m_textRender = 0;
    palette_update_flag = 0;
    m_paletteID = 0;
    m_paletteVersion = 0;
    m_pgsBuffer = new uint8_t[PG_BUFFER_SIZE];
    m_minLine = 0;
    m_maxLine = 0;
    if (sourceIsText)
    {
#ifdef _WIN32
#ifdef WIN32_DEBUG_FREETYPE
        m_textRender = new TextSubtitlesRenderFT;
#else
        m_textRender = new TextSubtitlesRenderWin32;
#endif
#else
        m_textRender = new TextSubtitlesRenderFT;
#endif
    }
}

TextToPGSConverter::~TextToPGSConverter()
{
    delete[] m_pgsBuffer;
    delete[] m_renderedData;
    delete m_textRender;
}

void TextToPGSConverter::enlargeCrop(int width, int height, int* newWidth, int* newHeight)
{
    *newWidth = width;
    *newHeight = height;
    if (width > 1280)
    {
        *newWidth = 1920;
        *newHeight = 1080;
    }
    else if (width > 720)
    {
        *newWidth = 1280;
        *newHeight = 720;
    }
    else if (fabs(m_videoFps - 25.0) < 0.1 || fabs(m_videoFps - 50.0) < 0.1)
    {
        *newWidth = 720;
        *newHeight = 576;
    }
    else
    {
        *newWidth = 720;
        *newHeight = 480;
    }
}

void TextToPGSConverter::setVideoInfo(int width, int height, double fps)
{
    enlargeCrop(width, height, &m_videoWidth, &m_videoHeight);
    // m_bottomOffset += m_videoHeight - height;
    // m_videoWidth = width;
    // m_videoHeight = height;
    m_videoFps = fps;
    if (m_textRender)
        m_textRender->setRenderSize(m_videoWidth, m_videoHeight);
    delete[] m_renderedData;
    m_renderedData = new uint8_t[(m_videoWidth + 16) * m_videoHeight];
}

double TextToPGSConverter::alignToGrid(double value)
{
    auto frameCnt =
        (int64_t)(value * m_videoFps + 0.5);  // how many frames have passed until this moment in time (rounded)
    return frameCnt / m_videoFps;
}

uint8_t TextToPGSConverter::color32To8(uint32_t* buff, uint32_t colorMask)
{
    // if (*buff == 0) // RESERVED_BACKGROUND_COLOR
    //	return 0xff;
    // else
    {
        YUVQuad yuv = RGBAToYUVA(*buff | colorMask);
        auto itr = m_paletteYUV.find(yuv);
        if (itr == m_paletteYUV.end())
        {
            if (m_paletteYUV.size() < 255)
            {
                pair<map<YUVQuad, uint8_t>::iterator, bool> rez =
                    m_paletteYUV.insert(std::make_pair(yuv, (uint8_t)m_paletteYUV.size()));
                return rez.first->second;
            }
            else
            {
                // find nearest color
                // if (m_paletteYUV.size() >= 256)
                THROW(ERR_COMMON, "Can't transform image to YUV: too many colors are used.");
            }
            // return m_paletteYUV[yuv];
        }
        else
            return itr->second;
    }
}

int TextToPGSConverter::getRepeatCnt(const uint32_t* pos, const uint32_t* end, uint32_t colorMask)
{
    int rez = 1;
    if (*pos == 0)
    {
        for (const uint32_t* cur = pos + 1; cur < end; cur++)
        {
            if (*cur == *pos)
                rez++;
            else
                break;
        }
    }
    else
    {
        const uint32_t rgbColor = *pos | colorMask;
        YUVQuad color = RGBAToYUVA(rgbColor);
        for (const uint32_t* cur = pos + 1; cur < end && *cur != 0; cur++)
        {
            uint32_t newRGBColor = *cur | colorMask;
            if (newRGBColor == rgbColor || RGBAToYUVA(newRGBColor) == color)
                rez++;
            else
                break;
        }
    }
    return rez;
}

YUVQuad TextToPGSConverter::RGBAToYUVA(uint32_t data)
{
    auto rgba = (RGBQUAD*)&data;
    YUVQuad rez;
    rez.Y = (int)(65.738 * rgba->rgbRed + 129.057 * rgba->rgbGreen + 25.064 * rgba->rgbBlue + 4224.0) >> 8;
    rez.Cr = (int)(112.439 * rgba->rgbRed - 94.154 * rgba->rgbGreen - 18.285 * rgba->rgbBlue + 32896.0) >> 8;
    rez.Cb = (int)(-37.945 * rgba->rgbRed - 74.494 * rgba->rgbGreen + 112.439 * rgba->rgbBlue + 32896.0) >> 8;
    rez.alpha = rgba->rgbReserved;
    return rez;
}

RGBQUAD TextToPGSConverter::YUVAToRGBA(const YUVQuad& yuv)
{
    RGBQUAD rez{};
    int tmp = (int)(298.082 * yuv.Y + 516.412 * yuv.Cb - 70742.016) >> 8;
    rez.rgbBlue = FFMAX(FFMIN(tmp, 255), 0);
    tmp = (int)(298.082 * yuv.Y - 208.120 * yuv.Cr - 100.291 * yuv.Cb + 34835.456) >> 8;
    rez.rgbGreen = FFMAX(FFMIN(tmp, 255), 0);
    tmp = (int)(298.082 * yuv.Y + 516.412 * yuv.Cr - 56939.776) >> 8;
    rez.rgbRed = FFMAX(FFMIN(tmp, 255), 0);
    rez.rgbReserved = yuv.alpha;
    return rez;
}

void TextToPGSConverter::reduceColors(uint8_t mask)
{
    mask = ~mask;
    uint32_t val = (mask << 24) + (mask << 16) + (mask << 8) + mask;
    auto dst = (uint32_t*)(m_textRender ? m_textRender->m_pData : m_imageBuffer);
    uint32_t* end = dst + m_videoWidth * m_videoHeight;
    for (; dst < end; ++dst) *dst &= val;
}

bool TextToPGSConverter::rlePack(uint32_t colorMask)
{
    try
    {
        m_paletteYUV.clear();
        // compress render buffer by RLE

        uint8_t* curPtr = m_renderedData;
        uint8_t* trimPos = m_renderedData;
        auto srcData = (uint32_t*)(m_textRender ? m_textRender->m_pData : m_imageBuffer);
        assert(srcData);
        m_rleLen = 0;
        m_minLine = INT_MAX;
        m_maxLine = 0;
        for (int y = 0; y < m_videoHeight; y++)
        {
            const uint32_t* srcLineEnd = srcData + m_videoWidth;
            const uint8_t* dstLineEnd = curPtr + m_videoWidth + 16;
            bool isEmptyLine = false;
            while (srcData < srcLineEnd)
            {
                int repCnt = getRepeatCnt(srcData, srcLineEnd, colorMask);

                if (repCnt == m_videoWidth)
                {
                    isEmptyLine = true;
                    if (curPtr == m_renderedData)
                    {
                        srcData += repCnt;
                        continue;
                    }
                }
                else
                {
                    m_minLine = FFMIN(m_minLine, y);
                    m_maxLine = FFMAX(m_maxLine, y);
                }

                uint8_t srcColor = color32To8(srcData, colorMask);
                assert(repCnt < 16384);
                if (srcColor)  // color exists
                {
                    if (repCnt > 2)
                    {
                        *curPtr++ = 0;
                        if (repCnt <= 63)
                            *curPtr++ = 0x80 + repCnt;
                        else
                        {
                            *curPtr++ = (uint8_t)(0xc0 + (repCnt >> 8));
                            *curPtr++ = (uint8_t)(repCnt & 0xff);
                        }
                        *curPtr++ = srcColor;
                        srcData += repCnt;
                    }
                    else
                    {  // simple color
                        *curPtr++ = srcColor;
                        srcData++;
                        if (repCnt == 2)
                        {
                            *curPtr++ = srcColor;
                            srcData++;
                        }
                    }
                }
                else  // zero color
                {
                    *curPtr++ = 0;
                    if (repCnt <= 63)
                        *curPtr++ = repCnt;
                    else
                    {
                        *curPtr++ = (uint8_t)0x40 + (repCnt >> 8);
                        *curPtr++ = (uint8_t)(repCnt & 0xff);
                    }
                    srcData += repCnt;
                }
            }
            if (m_minLine != INT_MAX)
            {
                *curPtr++ = 0;  // end of line signal
                *curPtr++ = 0;  // end of line signal
                if (curPtr >= dstLineEnd)
                {
                    THROW(ERR_COMMON, "Not enough RLE buffer for encoding picture (RLE line length > width + 16)");
                }
            }
            if (!isEmptyLine)
                trimPos = curPtr;
        }
        if (m_minLine == INT_MAX)
            m_minLine = m_maxLine = 0;
        m_rleLen = (int)(trimPos - m_renderedData);
        // sort by colors indexes
        m_paletteByColor.clear();
        for (std::map<YUVQuad, uint8_t>::const_iterator itr = m_paletteYUV.begin(); itr != m_paletteYUV.end(); ++itr)
            m_paletteByColor.insert(std::make_pair(itr->second, itr->first));
        assert(m_paletteByColor.size() == m_paletteYUV.size());
        return true;
    }
    catch (VodCoreException)
    {
        return false;
    }
}

int TextToPGSConverter::renderedHeight() const { return m_maxLine - m_minLine + 1; }

int TextToPGSConverter::maxLine() const { return m_maxLine; }

int TextToPGSConverter::minLine() const { return m_minLine; }

TextToPGSConverter::Palette TextToPGSConverter::buildPalette(float opacity)
{
    if (opacity == 1.0)
        return m_paletteByColor;
    Palette result = m_paletteByColor;
    for (auto itr = result.begin(); itr != result.end(); ++itr)
        itr->second.alpha = FFMIN(255, uint8_t(float(itr->second.alpha) * opacity + 0.5));
    return result;
}

float toCurve(float value)
{
    // float result = pow(value, 1.5f);
    float result = value * sqrt(value);  // same as pow 1.5, reduce binary size
    return result;
}

uint8_t* TextToPGSConverter::doConvert(std::string& text, const TextAnimation& animation, double inTimeSec,
                                       double outTimeSec, uint32_t& dstBufSize)
{
    bool forced = m_textRender->rasterText(text);
    inTimeSec = alignToGrid(inTimeSec);
    outTimeSec = alignToGrid(outTimeSec);

    auto inTimePTS = (int64_t)(inTimeSec * 90000);
    auto outTimePTS = (int64_t)(outTimeSec * 90000);

    uint32_t mask = 0;
    int step = 0;
    while (!rlePack(mask))
    {
        // reduce colors
        auto tmp = (uint8_t*)&mask;
        int idx = step++ % 4;
        tmp[idx] <<= 1;
        tmp[idx]++;
    }

    if (m_rleLen == 0)
        return 0;  // empty text

    int objectWindowHeight = FFMAX(0, renderedHeight());
    int objectWindowTop = FFMAX(0, m_textRender->m_height - objectWindowHeight - m_bottomOffset);

    int fadeInFrames = (int)(animation.fadeInDuration * m_videoFps + 0.5);
    int fadeOutFrames = (int)(animation.fadeOutDuration * m_videoFps + 0.5);
    fadeInFrames++;
    fadeOutFrames++;

    float opacityInDelta = 1.0f / fadeInFrames;
    float opacityOutDelta = 1.0f / fadeOutFrames;
    float opacity = opacityInDelta;

    double decodedObjectSize = (m_maxLine - m_minLine + 1) * m_videoWidth;
    auto compositionDecodeTime = (int64_t)(90000.0 * decodedObjectSize / PIXEL_DECODING_RATE + 0.999);
    auto windowsTransferTime = (int64_t)(90000.0 * decodedObjectSize / PIXEL_COMPOSITION_RATE + 0.999);

    const auto PLANEINITIALIZATIONTIME =
        (int64_t)(90000.0 * (m_videoWidth * m_videoHeight) / PIXEL_COMPOSITION_RATE + 0.999);
    const int64_t PRESENTATION_DTS_DELTA = PLANEINITIALIZATIONTIME + windowsTransferTime;

    // 1. show text
    uint8_t* curPos = m_pgsBuffer;
    palette_update_flag = 0;
    m_paletteID = 0;
    m_paletteVersion = 0;

    curPos += composePresentationSegment(curPos, CompositionMode::Start, inTimePTS, inTimePTS - PRESENTATION_DTS_DELTA,
                                         objectWindowTop, true, forced);
    curPos += composeWindowDefinition(curPos, inTimePTS - windowsTransferTime, inTimePTS - PRESENTATION_DTS_DELTA,
                                      objectWindowTop, objectWindowHeight);
    curPos += composePaletteDefinition(buildPalette(toCurve(opacity)), curPos, inTimePTS - PRESENTATION_DTS_DELTA,
                                       inTimePTS - PRESENTATION_DTS_DELTA);
    int64_t odfPTS = inTimePTS - PRESENTATION_DTS_DELTA + compositionDecodeTime;
    curPos += composeObjectDefinition(curPos, odfPTS, inTimePTS - PRESENTATION_DTS_DELTA, m_minLine, m_maxLine, true);
    curPos += composeEnd(curPos, odfPTS, odfPTS);

    // 2.1 fade in palette
    double fpsPts = 90000.0 / m_videoFps;
    auto updateTime = (int64_t)alignToGrid(inTimePTS + fpsPts);

    auto lastAnimateTime = (int64_t)alignToGrid(outTimePTS - fpsPts);
    opacity += opacityInDelta;
    while (updateTime <= lastAnimateTime + FLOAT_EPS && opacity <= 1.0 + FLOAT_EPS)
    {
        palette_update_flag = 1;
        m_paletteVersion++;
        curPos += composePresentationSegment(curPos, CompositionMode::Update, updateTime,
                                             updateTime - windowsTransferTime, objectWindowTop, true, forced);
        curPos += composePaletteDefinition(buildPalette(toCurve(opacity)), curPos, updateTime - windowsTransferTime,
                                           updateTime - windowsTransferTime);
        curPos += composeEnd(curPos, updateTime - 90, updateTime - 90);

        updateTime = (int64_t)alignToGrid(updateTime + fpsPts);
        opacity += opacityInDelta;
    }

    // 2.2 fade out palette
    updateTime = (int64_t)alignToGrid(FFMAX(updateTime, outTimePTS - (fadeOutFrames - 1) * fpsPts));
    opacity = 1.0f - opacityOutDelta;
    while (updateTime < outTimePTS - FLOAT_EPS)
    {
        palette_update_flag = 1;
        m_paletteVersion++;
        curPos += composePresentationSegment(curPos, CompositionMode::Update, (int64_t)updateTime,
                                             (int64_t)updateTime - windowsTransferTime, objectWindowTop, true, forced);
        curPos +=
            composePaletteDefinition(buildPalette(toCurve(opacity)), curPos, (int64_t)updateTime - windowsTransferTime,
                                     (int64_t)updateTime - windowsTransferTime);
        curPos += composeEnd(curPos, (int64_t)updateTime - 90, (int64_t)updateTime - 90);

        updateTime = (int64_t)alignToGrid(updateTime + fpsPts);
        opacity -= opacityOutDelta;
    }

    // 3. hide text

    palette_update_flag = 0;
    curPos += composePresentationSegment(curPos, CompositionMode::Finish, outTimePTS,
                                         outTimePTS - windowsTransferTime - 90, objectWindowTop, true, false);
    curPos += composeWindowDefinition(curPos, outTimePTS - windowsTransferTime, outTimePTS - windowsTransferTime - 90,
                                      objectWindowTop, objectWindowHeight);
    curPos += composeEnd(curPos, outTimePTS - 90, outTimePTS - 90);

    assert(curPos - m_pgsBuffer < PG_BUFFER_SIZE);

    dstBufSize = (uint32_t)(curPos - m_pgsBuffer);
    return m_pgsBuffer;
}

long TextToPGSConverter::composePresentationSegment(uint8_t* buff, CompositionMode mode, int64_t pts, int64_t dts,
                                                    int top, bool needPGHeader, bool forced)
{
    uint8_t* curPos = buff;
    if (needPGHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = PCS_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;

    curPos += composeVideoDescriptor(curPos);
    curPos += composeCompositionDescriptor(curPos, m_composition_number++,
                                           mode == CompositionMode::Start ? EPOTH_START : EPOTH_NORMAL);
    *curPos++ = palette_update_flag << 7;                 // palette_update_flag = 0 and 7 reserved bits
    *curPos++ = m_paletteID;                              // paletteID ref
    *curPos++ = mode != CompositionMode::Finish ? 1 : 0;  // number_of_composition_objects
    // composition object
    if (mode != CompositionMode::Finish)
    {
        *curPos++ = 0;  // objectID ref
        *curPos++ = 0;  // objectID ref
        *curPos++ = 0;  // windowID ref
        if (forced)
            *curPos++ = 0x40;  // object_cropped_flag = false, forced_on_flag = true
        else
            *curPos++ = 0;  // object_cropped_flag = false, forced_on_flag = false
        AV_WB16(curPos, 0);
        curPos += 2;  // object horizontal position = 0
        AV_WB16(curPos, top);
        curPos += 2;  // object vertical position
    }

    AV_WB16(startPos - 2, (uint16_t)(curPos - startPos));
    return (long)(curPos - buff);
}

long TextToPGSConverter::composeVideoDescriptor(uint8_t* buff)
{
    uint8_t* curPos = buff;
    AV_WB16(curPos, m_videoWidth);
    curPos += 2;
    AV_WB16(curPos, m_videoHeight);
    curPos += 2;
    *curPos++ = PGSStreamReader::calcFpsIndex(m_videoFps) << 4;
    return (long)(curPos - buff);
}

long TextToPGSConverter::composeWindowDefinition(uint8_t* buff, int64_t pts, int64_t dts, int top, int height,
                                                 bool needPgHeader)
{
    uint8_t* curPos = buff;
    if (needPgHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = WINDOWS_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;
    *curPos++ = 1;  // number of windows
    curPos += composeWindow(curPos, top, height);
    AV_WB16(startPos - 2, (uint16_t)(curPos - startPos));
    return (long)(curPos - buff);
}

long TextToPGSConverter::composeCompositionDescriptor(uint8_t* buff, uint16_t number, uint8_t state)
{
    uint8_t* curPos = buff;
    AV_WB16(curPos, number);
    curPos += 2;
    *curPos++ = state << 6;
    return (long)(curPos - buff);
}

long TextToPGSConverter::composeWindow(uint8_t* buff, int top, int height)
{
    uint8_t* curPos = buff;
    *curPos++ = 0;  // window ID
    AV_WB16(curPos, 0);
    curPos += 2;
    AV_WB16(curPos, top);
    curPos += 2;
    AV_WB16(curPos, m_videoWidth);
    curPos += 2;
    AV_WB16(curPos, height);
    curPos += 2;
    return (long)(curPos - buff);
}

long TextToPGSConverter::composePaletteDefinition(const Palette& palette, uint8_t* buff, int64_t pts, int64_t dts,
                                                  bool needPgHeader)
{
    uint8_t* curPos = buff;
    if (needPgHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = PALETTE_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;
    *curPos++ = m_paletteID;       // palette ID
    *curPos++ = m_paletteVersion;  // palette version number
    for (auto itr = palette.begin(); itr != palette.end(); ++itr)
    {
        *curPos++ = itr->first;
        *curPos++ = itr->second.Y;
        *curPos++ = itr->second.Cr;
        *curPos++ = itr->second.Cb;
        *curPos++ = itr->second.alpha;
    }
    AV_WB16(startPos - 2, (uint16_t)(curPos - startPos));  // correct length field
    return (long)(curPos - buff);
}

long TextToPGSConverter::composeObjectDefinition(uint8_t* buff, int64_t pts, int64_t dts, int firstLine, int lastLine,
                                                 bool needPgHeader)
{
    std::vector<uint8_t*> seqPos;

    const uint8_t* srcData = m_renderedData;
    int srcProcessed = 0;
    int blocks = 0;
    uint8_t* curPos = buff;
    uint8_t* sizePos = buff;
    do
    {
        if (needPgHeader)
            curPos += writePGHeader(curPos, pts, dts);
        *curPos++ = OBJECT_DEF_SEGMENT;
        curPos += 2;  // skip length field
        uint8_t* fragmentStart = curPos;
        *curPos++ = 0;  // objectID
        *curPos++ = 0;  // objectID
        *curPos++ = 0;  // object version number
        seqPos.push_back(curPos);
        *curPos++ = 0;  // 0xc0; // sequence descriptor: first=true, last=true

        if (blocks == 0)
        {
            sizePos = curPos;
            // object data header
            curPos += 3;  // skip total size
            AV_WB16(curPos, m_videoWidth);
            curPos += 2;
            AV_WB16(curPos, lastLine - firstLine + 1);
            curPos += 2;
        }

        int MAX_PG_PACKET = 65515;
        if (blocks == 0)
            MAX_PG_PACKET -= 7;
        int size = FFMIN(m_rleLen - srcProcessed, MAX_PG_PACKET);
        memcpy(curPos, srcData + srcProcessed, size);
        srcProcessed += size;
        curPos += size;

        AV_WB16(fragmentStart - 2, (uint16_t)(curPos - fragmentStart));  // correct length field
        blocks++;
    } while (srcProcessed < m_rleLen);
    AV_WB24(sizePos, m_rleLen + 4);  // object len
    if (seqPos.size() > 0)
    {
        *(seqPos[0]) |= 0x80;
        *(seqPos[seqPos.size() - 1]) |= 0x40;
    }
    return (long)(curPos - buff);
}

long TextToPGSConverter::composeEnd(uint8_t* buff, int64_t pts, int64_t dts, bool needPgHeader)
{
    uint8_t* curPos = buff;
    if (needPgHeader)
        curPos += writePGHeader(curPos, pts, dts);
    *curPos++ = END_DEF_SEGMENT;
    curPos += 2;  // skip length field
    uint8_t* startPos = curPos;
    AV_WB16(startPos - 2, (uint16_t)(curPos - startPos));
    return (long)(curPos - buff);
}

long TextToPGSConverter::writePGHeader(uint8_t* buff, int64_t pts, int64_t dts)
{
    if (dts > pts)
        dts = pts;
    *buff++ = 'P';
    *buff++ = 'G';
    auto data = (uint32_t*)buff;
    *data++ = my_htonl((uint32_t)pts);
    if (dts != pts)
        *data = my_htonl((uint32_t)dts);
    else
        *data = 0;
    return 10;
}
