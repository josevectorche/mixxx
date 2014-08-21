/* -*- mode:C++; indent-tabs-mode:t; tab-width:8; c-basic-offset:4; -*- */
/***************************************************************************
                          soundsourceffmpeg.cpp -  ffmpeg decoder
                             -------------------
    copyright            : (C) 2007 by Cedric GESTES
                           (C) 2012-2014 by Tuukka Pasanen
    email                : tuukka.pasanen@ilmi.fi

    This one tested with FFMPEG 0.10/0.11/1.0/1.1/1.2/2,0/2,1/GIT
                         Libav  0.8/9/GIT
    FFMPEG below 0.10 WON'T work. If you like to it work you can
    allways send a patch but it's mostly not worth it!
***************************************************************************/

/***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************/

#include "trackinfoobject.h"
#include "soundsourceffmpeg.h"

//#ifdef __WINDOWS__
//#include <io.h>
//#include <fcntl.h>
//#endif

#include <QtDebug>
#include <QBuffer>


static QMutex ffmpegmutex;

#define SOUNDSOURCEFFMPEG_CACHESIZE 1000

SoundSourceFFmpeg::SoundSourceFFmpeg(QString filename)
    : Mixxx::SoundSource(filename)
    , m_qFilename(filename) {
    m_iAudioStream = -1;
    filelength = -1;
    m_pFormatCtx = NULL;
    m_pIformat = NULL;
    m_pCodecCtx = NULL;
    m_pCodec = NULL;
    m_iOffset = 0;
    m_iSeekOffset = 0;
    m_bIsSeeked = FALSE;
    m_iReadedBytes = 0;
    m_iNextMixxxPCMPoint = -1;
    m_pResample = NULL;
    m_fMixxBytePosition = 0;
    m_iLastFirstFfmpegByteOffset = 0;
    m_iCurrentMixxTs = 0;
    m_lCacheBytePos = 0;
    m_lCacheStartByte = 0;
    m_lCacheEndByte = 0;
    m_lCacheLastPos = 0;
    m_SCache.clear();

    this->setType(filename.section(".",-1).toLower());
}

SoundSourceFFmpeg::~SoundSourceFFmpeg() {
    clearCache();

    if (m_pCodecCtx != NULL) {
        avcodec_close(m_pCodecCtx);
        avformat_close_input(&m_pFormatCtx);
    }

    if (m_pResample != NULL) {
        delete m_pResample;
    }
}

AVCodecContext *SoundSourceFFmpeg::getCodecContext() {
    return m_pCodecCtx;
}

AVFormatContext *SoundSourceFFmpeg::getFormatContext() {
    return m_pFormatCtx;
}

int SoundSourceFFmpeg::getAudioStreamIndex() {
    return m_iAudioStream;
}

void SoundSourceFFmpeg::lock() {
    ffmpegmutex.lock();
}

void SoundSourceFFmpeg::unlock() {
    ffmpegmutex.unlock();
}

bool SoundSourceFFmpeg::clearCache() {
    struct ffmpegCacheObject *l_SRmObj = NULL;

    while (m_SCache.size() > 0) {
        l_SRmObj = m_SCache[0];
        m_SCache.remove(0);
        free(l_SRmObj->bytes);
        free(l_SRmObj);
    }


    return true;
}

bool SoundSourceFFmpeg::readFramesToCache(unsigned int count, int64_t offset) {
    unsigned int l_iCount = 0;
    int32_t l_iRet = 0;
    AVPacket l_SPacket;
    AVFrame *l_pFrame = NULL;
    bool l_iStop = false;
    int l_iFrameFinished = 0;
    struct ffmpegCacheObject *l_SObj = NULL;
    struct ffmpegCacheObject *l_SRmObj = NULL;
    int l_iError = 0;

    l_iCount = count;

    l_SPacket.data = NULL;
    l_SPacket.size = 0;

    while (l_iCount > 0) {
        av_init_packet(&l_SPacket);
        l_pFrame = avcodec_alloc_frame();

        if (av_read_frame(m_pFormatCtx, &l_SPacket) >= 0) {
            if (l_SPacket.stream_index==m_iAudioStream) {
                l_iRet = avcodec_decode_audio4(m_pCodecCtx,l_pFrame,&l_iFrameFinished,
                                               &l_SPacket);

                if (l_iRet <= 0) {
                    // An error or EOF occured,index break out and return what
                    // we have so far.
                    qDebug() << "EOF!";
                    l_iStop = true;
                } else {
                    l_iRet = 0;
                    l_SObj = (struct ffmpegCacheObject *)malloc(sizeof(struct ffmpegCacheObject));
                    if (l_SObj == NULL) {
                        qDebug() << "SoundSourceFFmpeg::readFramesToCache: Not enough memory!";
                        return false;
                    }
                    memset(l_SObj, 0x00, sizeof(struct ffmpegCacheObject));
                    l_iRet = m_pResample->reSample(l_pFrame, &l_SObj->bytes);

                    if (l_iRet > 0) {
                        if (m_SCache.size() >= (SOUNDSOURCEFFMPEG_CACHESIZE - 10)) {
                            //l_SRmObj = m_SCache.takeFirst();
                            l_SRmObj = m_SCache[0];
                            m_SCache.remove(0);
                            free(l_SRmObj->bytes);
                            free(l_SRmObj);
                        }

                        m_SCache.append(l_SObj);
                        l_SObj->startByte = m_lCacheBytePos / 2;
                        l_SObj->length = l_iRet / 2;
                        m_lCacheBytePos += l_iRet;

                        if (offset < 0 || offset <= (m_lCacheBytePos / 2)) {
                            l_iCount --;
                        }
                    } else {
                        qDebug() <<
                                 "SoundSourceFFmpeg::readFramesToCache: General error in audio decode:" <<
                                 l_iRet;
                    }
                }

#if LIBAVCODEC_VERSION_INT > 3544932
                if (l_SPacket.data != NULL) {
                    av_free(l_SPacket.data);
                }
#endif
                l_SPacket.data = NULL;
                l_SPacket.size = 0;

            } else {
                l_iError ++;
                if (l_iError == 5) {
                    // Stream end and we couldn't read enough frames
                    l_iStop = true;
                }
            }

            av_frame_unref(l_pFrame);

            l_pFrame = NULL;

            if (l_iStop == true) {
                return false;
            }
        } else {
            qDebug() << "SoundSourceFFmpeg::readFramesToCache: Packet too big or File end";
            break;
        }
    }
    l_SObj = m_SCache.first();
    m_lCacheStartByte = l_SObj->startByte;
    l_SObj = m_SCache.last();
    m_lCacheEndByte = (l_SObj->startByte + l_SObj->length);

    if ( !l_iCount ) {
        return true;
    } else {
        return false;
    }
}

bool SoundSourceFFmpeg::getBytesFromCache(char *buffer, uint64_t offset,
        uint64_t size) {
    struct ffmpegCacheObject *l_SObj = NULL;
    uint32_t l_lPos = 0;
    uint32_t l_lLeft = 0;
    uint32_t l_lOffset = 0;
    uint32_t l_lBytesToCopy = 0;

    if (offset >= m_lCacheStartByte) {
        if (m_lCacheLastPos == 0) {
            // lock();
            m_lCacheLastPos = m_SCache.size() - 1;
            // unlock();

        }
        for (l_lPos = m_lCacheLastPos; l_lPos > 0; l_lPos --) {
            // lock();
            l_SObj = m_SCache[l_lPos];
            // unlock();

            if ((l_SObj->startByte + l_SObj->length) < offset) {
                break;
            }
        }

        l_SObj = m_SCache[l_lPos];

        l_lLeft = (size * 2);
        memset(buffer, 0x00, l_lLeft);
        while (l_lLeft > 0) {

            if (l_SObj == NULL || (l_lPos + 5) > (unsigned int)m_SCache.size()) {
                offset = l_SObj->startByte;
                if (readFramesToCache(50, -1) == false) {
                    return false;
                }
                for (l_lPos = (m_SCache.size() - 50); l_lPos > 0; l_lPos --) {
                    l_SObj = m_SCache[l_lPos];
                    if ((l_SObj->startByte + l_SObj->length) < offset) {
                        break;
                    }
                }
                l_SObj = m_SCache[l_lPos];
                continue;
            }

            if (l_SObj->startByte < offset) {
                l_lOffset = (offset - l_SObj->startByte) * 2;
            }

            if (l_lOffset > (l_SObj->length * 2)) {
                l_SObj = m_SCache[++ l_lPos];
                continue;
            }

            if (l_lLeft > (l_SObj->length * 2)) {
                l_lBytesToCopy = ((l_SObj->length * 2)  - l_lOffset);
                memcpy(buffer, (l_SObj->bytes + l_lOffset), l_lBytesToCopy);
                l_lOffset = 0;
                buffer += l_lBytesToCopy;
                l_lLeft -= l_lBytesToCopy;
            } else {
                memcpy(buffer, l_SObj->bytes, l_lLeft);
                l_lLeft = 0;
            }

            l_SObj = m_SCache[++ l_lPos];
        }

        m_lCacheLastPos = --l_lPos;
        return true;
    }

    return false;
}

double SoundSourceFFmpeg::convertPtsToByteOffset(double pts,
        const AVRational &ffmpegtime) {
    return (pts / (double)ffmpegtime.den * (double)this->getSampleRate() *
            (double)2.);
}

double SoundSourceFFmpeg::convertByteOffsetToPts(double byteoffset,
        const AVRational &ffmpegtime) {
    return (byteoffset / (double)this->getSampleRate() / (double)2.) *
           (double)ffmpegtime.den;
}

int64_t SoundSourceFFmpeg::convertPtsToByteOffsetOld(int64_t pts,
        const AVRational &ffmpegbase) {
    int64_t l_lReturnValue = 0;

    l_lReturnValue = round(convertPtsToByteOffset(pts, ffmpegbase));

    if ((l_lReturnValue % 4) != 0) {
        l_lReturnValue += 4 - (l_lReturnValue % 4);
    }

    return l_lReturnValue;
}

int64_t SoundSourceFFmpeg::convertByteOffsetToPtsOld(int64_t byteoffset,
        const AVRational &ffmpegtime) {
    int64_t l_lReturnValue = 0;
    l_lReturnValue = round(convertByteOffsetToPts(byteoffset, ffmpegtime));
    return l_lReturnValue + (l_lReturnValue % 2);
}

Result SoundSourceFFmpeg::open() {
    unsigned int i;
    AVDictionary *l_iFormatOpts = NULL;

    m_iOffset = 0;
    QByteArray qBAFilename = m_qFilename.toLocal8Bit();

    // Initialize FFMPEG
    // FFmpegInit();

    qDebug() << "New SoundSourceFFmpeg :" << qBAFilename;

    m_pFormatCtx = avformat_alloc_context();

// Enable this to use old slow MP3 Xing TOC
#if LIBAVCODEC_VERSION_INT > 3544932
    qDebug() << "Using MP3 Xing TOC if needed";
    av_dict_set(&l_iFormatOpts, "usetoc", "0", 0);
#endif

    m_pFormatCtx->max_analyze_duration = 999999999;
    // lock();
    // Open file and make m_pFormatCtx
    if (avformat_open_input(&m_pFormatCtx, qBAFilename.constData(), NULL,
                            &l_iFormatOpts)!=0) {
        qDebug() << "av_open_input_file: cannot open" << qBAFilename;
        return ERR;
    }
    // unlock();

#if LIBAVCODEC_VERSION_INT > 3544932
    av_dict_free(&l_iFormatOpts);
#endif

    // lock();
    // Retrieve stream information
    if (avformat_find_stream_info(m_pFormatCtx, NULL)<0) {
        qDebug() << "av_find_stream_info: cannot open" << qBAFilename;
        return ERR;
    }
    // unlock();

    //debug only (Enable if needed)
    //av_dump_format(m_pFormatCtx, 0, qBAFilename.constData(), false);

    // Find the first video stream
    m_iAudioStream=-1;

    for (i=0; i<m_pFormatCtx->nb_streams; i++)
        if (m_pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO) {
            m_iAudioStream=i;
            break;
        }
    if (m_iAudioStream==-1) {
        qDebug() << "ffmpeg: cannot find an audio stream: cannot open"
                 << qBAFilename;
        return ERR;
    }

    // Get a pointer to the codec context for the video stream
    m_pCodecCtx=m_pFormatCtx->streams[m_iAudioStream]->codec;

    // Find the decoder for the audio stream
    if (!(m_pCodec=avcodec_find_decoder(m_pCodecCtx->codec_id))) {
        qDebug() << "ffmpeg: cannot find a decoder for" << qBAFilename;
        return ERR;
    }

    // qDebug() << "ffmpeg: opening the audio codec";
    //avcodec_open is not thread safe
    lock();
    if (avcodec_open2(m_pCodecCtx, m_pCodec, NULL)<0) {
        qDebug() << "ffmpeg:  cannot open" << qBAFilename;
        return ERR;
    }
    unlock();

    m_pResample = new EncoderFfmpegResample(m_pCodecCtx);
    m_pResample->open(m_pCodecCtx->sample_fmt, AV_SAMPLE_FMT_S16);

    this->setChannels(m_pCodecCtx->channels);
    this->setSampleRate(m_pCodecCtx->sample_rate);

    qDebug() << "ffmpeg: Samplerate: " << this->getSampleRate() << ", Channels: " <<
             this->getChannels() << "\n";
    if (this->getChannels() > 2) {
        qDebug() << "ffmpeg: No support for more than 2 channels!";
        return ERR;
    }
    filelength = (long int) ((double)m_pFormatCtx->duration * 2 / AV_TIME_BASE *
                             this->getSampleRate());

    return OK;
}

long SoundSourceFFmpeg::seek(long filepos) {
    int ret = 0;

    if (filepos == 0 || (unsigned long) filepos < m_lCacheStartByte) {
        ret = avformat_seek_file(m_pFormatCtx,
                                 m_iAudioStream,
                                 0,
                                 32767 * 2,
                                 32767 * 2,
                                 AVSEEK_FLAG_BACKWARD);

        if (ret < 0) {
            qDebug() << "SoundSourceFFmpeg::seek: Can't seek to 0 byte!";
            return -1;
        }

        clearCache();
        m_SCache.clear();
        m_lCacheStartByte = 0;
        m_lCacheEndByte = 0;
        m_lCacheLastPos = 0;
        m_lCacheBytePos = 0;
        if (filepos == 0) {
            readFramesToCache((SOUNDSOURCEFFMPEG_CACHESIZE - 50), -1);
        } else {
            readFramesToCache((SOUNDSOURCEFFMPEG_CACHESIZE / 2), filepos);
        }
    }

    if (m_lCacheEndByte <= (unsigned long) filepos) {
        readFramesToCache(100, filepos);
    }

    m_iCurrentMixxTs = filepos;

    m_bIsSeeked = TRUE;

    return filepos;
}

unsigned int SoundSourceFFmpeg::read(unsigned long size,
                                     const SAMPLE * destination) {

    if (m_SCache.size() == 0) {
        // Make sure we allways start at begining and cache have some
        // material that we can consume.
        seek(0);
        m_bIsSeeked = FALSE;
    }

    getBytesFromCache((char *)destination, m_iCurrentMixxTs, size);


    //  As this is also Hack
    // If we don't seek like we don't on analyzer.. keep
    // place in mind..
    if (m_bIsSeeked == FALSE) {
        m_iCurrentMixxTs += size;
    }

    m_bIsSeeked = FALSE;
    return size;

}

Result SoundSourceFFmpeg::parseHeader() {
    qDebug() << "ffmpeg: SoundSourceFFmpeg::parseHeader" << m_qFilename;
    QByteArray qBAFilename = m_qFilename.toLocal8Bit();

    AVFormatContext * FmtCtx = avformat_alloc_context();
    AVCodecContext * CodecCtx;
    AVDictionaryEntry *FmtTag = NULL;
    unsigned int i;
    AVDictionary *l_iFormatOpts = NULL;

    // Enable this to use old slow MP3 Xing TOC
#ifndef CODEC_ID_MP3
    if (LIBAVFORMAT_VERSION_INT > 3540580) {
        av_dict_set(&l_iFormatOpts, "usetoc", "0", 0);
    }
#endif
    lock();
    if (avformat_open_input(&FmtCtx, qBAFilename.constData(), NULL,
                            &l_iFormatOpts) !=0) {
        qDebug() << "av_open_input_file: cannot open" << qBAFilename.constData();
        return ERR;
    }

#ifndef CODEC_ID_MP3
    if (LIBAVFORMAT_VERSION_INT > 3540580 && l_iFormatOpts != NULL) {
        av_dict_free(&l_iFormatOpts);
    }
#endif

    FmtCtx->max_analyze_duration = 999999999;

    // Retrieve stream information
    if (avformat_find_stream_info(FmtCtx, NULL)<0) {
        qDebug() << "av_find_stream_info: Can't find metadata" <<
                 qBAFilename.constData();
        return ERR;
    }
    unlock();
    for (i=0; i<FmtCtx->nb_streams; i++)
        if (FmtCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO) {
            m_iAudioStream=i;
            break;
        }
    if (m_iAudioStream==-1) {
        qDebug() << "cannot find an audio stream: Can't find stream" <<
                 qBAFilename.constData();
        return ERR;
    }

    // Get a pointer to the codec context for the video stream
    CodecCtx=FmtCtx->streams[m_iAudioStream]->codec;

    while ((FmtTag = av_dict_get(FmtCtx->metadata, "", FmtTag,
                                 AV_DICT_IGNORE_SUFFIX))) {
        QString strValue (QString::fromUtf8 (FmtTag->value));

        if (!strncmp(FmtTag->key, "artist", 7)) {
            this->setArtist(strValue);
        } else if (!strncmp(FmtTag->key, "album", 5)) {
            this->setAlbum(strValue);
        } else if (!strncmp(FmtTag->key, "date", 4)) {
            this->setYear(strValue);
        } else if (!strncmp(FmtTag->key, "genre", 5)) {
            this->setGenre(strValue);
        } else if (!strncmp(FmtTag->key, "title", 5)) {
            this->setTitle(strValue);
        }


    }

    while ((FmtTag = av_dict_get(FmtCtx->streams[m_iAudioStream]->metadata, "",
                                 FmtTag, AV_DICT_IGNORE_SUFFIX))) {
        // Convert the value from UTF-8.
        QString strValue (QString::fromUtf8 (FmtTag->value));

        if (!strncmp(FmtTag->key, "ARTIST", 7)) {
            this->setArtist(strValue);
        } else if (!strncmp(FmtTag->key, "ALBUM", 5)) {
            this->setAlbum(strValue);
        } else if (!strncmp(FmtTag->key, "YEAR", 4)) {
            this->setYear(strValue);
        } else if (!strncmp(FmtTag->key, "GENRE", 5)) {
            this->setGenre(strValue);
        } else if (!strncmp(FmtTag->key, "TITLE", 5)) {
            this->setTitle(strValue);
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_TRACK_PEAK", 20)) {
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_TRACK_GAIN", 20)) {
            this->parseReplayGainString (strValue);
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_ALBUM_PEAK", 20)) {
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_ALBUM_GAIN", 20)) {
        }


    }

    this->setType(m_qFilename.section(".",-1).toLower());
    this->setDuration(FmtCtx->duration / AV_TIME_BASE);
    this->setBitrate((int)(CodecCtx->bit_rate / 1000));
    this->setSampleRate(CodecCtx->sample_rate);
    this->setChannels(CodecCtx->channels);
    lock();
    avformat_close_input(&FmtCtx);
    unlock();
    return OK;
}

inline long unsigned SoundSourceFFmpeg::length() {
    return filelength;
}

QList<QString> SoundSourceFFmpeg::supportedFileExtensions() {
    QList<QString> list;
    AVInputFormat *l_SInputFmt  = NULL;

    while ((l_SInputFmt = av_iformat_next(l_SInputFmt))) {
        if (l_SInputFmt->name == NULL) {
            break;
        }

        // qDebug() << l_SInputFmt->name;

        if (!strcmp(l_SInputFmt->name, "flac")) {
            list.push_back("flac");
            qDebug() << "FFPEG Decode: FLAC";
        } else if (!strcmp(l_SInputFmt->name, "ogg")) {
            list.push_back("ogg");
            qDebug() << "FFMPEG Decode: Ogg/Vorbis";
        } else if (!strcmp(l_SInputFmt->name, "mov,mp4,m4a,3gp,3g2,mj2")) {
            list.push_back("m4a");
            qDebug() << "FFMPEG Decode: Apple m4a";
        } else if (!strcmp(l_SInputFmt->name, "mp4")) {
            list.push_back("mp4");
            qDebug() << "FFMPEG Decode: Mp4";
        } else if (!strcmp(l_SInputFmt->name, "mp3")) {
            list.push_back("mp3");
            qDebug() << "FFMPEG Decode: Mp3";
        } else if (!strcmp(l_SInputFmt->name, "aac")) {
            list.push_back("aac");
            qDebug() << "FFMPEG Decode: Apple AAC";
        } else if (!strcmp(l_SInputFmt->name, "opus") ||
                   !strcmp(l_SInputFmt->name, "libopus")) {
            list.push_back("opus");
            qDebug() << "FFMPEG Decode: Opus";
        } else if (!strcmp(l_SInputFmt->name, "wma")) {
            list.push_back("xwma");
            qDebug() << "FFMPEG Decode: WMA";
        }
    }

    return list;
}
