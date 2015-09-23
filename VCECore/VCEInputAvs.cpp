﻿//  -----------------------------------------------------------------------------------------
//    VCEEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  -----------------------------------------------------------------------------------------

#include <io.h>
#include <fcntl.h>
#include "VCEInputAvs.h"
#include "VCEUtil.h"

#if ENABLE_AVISYNTH_READER

VCEInputAvs::VCEInputAvs() : m_sAVSenv(nullptr), m_sAVSclip(nullptr), m_sAVSinfo(nullptr) {
    m_strReaderName = _T("avs");
    memset(&m_sAvisynth, 0, sizeof(m_sAvisynth));
}

VCEInputAvs::~VCEInputAvs() {
    Terminate();
}

void VCEInputAvs::release_avisynth() {
    if (m_sAvisynth.h_avisynth)
        FreeLibrary(m_sAvisynth.h_avisynth);

    memset(&m_sAvisynth, 0, sizeof(m_sAvisynth));
}

AMF_RESULT VCEInputAvs::load_avisynth() {
    release_avisynth();

    if (NULL == (m_sAvisynth.h_avisynth = (HMODULE)LoadLibrary(_T("avisynth.dll")))
        || NULL == (m_sAvisynth.invoke = (func_avs_invoke)GetProcAddress(m_sAvisynth.h_avisynth, "avs_invoke"))
        || NULL == (m_sAvisynth.take_clip = (func_avs_take_clip)GetProcAddress(m_sAvisynth.h_avisynth, "avs_take_clip"))
        || NULL == (m_sAvisynth.create_script_environment = (func_avs_create_script_environment)GetProcAddress(m_sAvisynth.h_avisynth, "avs_create_script_environment"))
        || NULL == (m_sAvisynth.delete_script_environment = (func_avs_delete_script_environment)GetProcAddress(m_sAvisynth.h_avisynth, "avs_delete_script_environment"))
        || NULL == (m_sAvisynth.get_frame = (func_avs_get_frame)GetProcAddress(m_sAvisynth.h_avisynth, "avs_get_frame"))
        || NULL == (m_sAvisynth.get_version = (func_avs_get_version)GetProcAddress(m_sAvisynth.h_avisynth, "avs_get_version"))
        || NULL == (m_sAvisynth.get_video_info = (func_avs_get_video_info)GetProcAddress(m_sAvisynth.h_avisynth, "avs_get_video_info"))
        || NULL == (m_sAvisynth.release_clip = (func_avs_release_clip)GetProcAddress(m_sAvisynth.h_avisynth, "avs_release_clip"))
        || NULL == (m_sAvisynth.release_value = (func_avs_release_value)GetProcAddress(m_sAvisynth.h_avisynth, "avs_release_value"))
        || NULL == (m_sAvisynth.release_video_frame = (func_avs_release_video_frame)GetProcAddress(m_sAvisynth.h_avisynth, "avs_release_video_frame")))
        return AMF_NOT_FOUND;
    return AMF_OK;
}

AMF_RESULT VCEInputAvs::init(shared_ptr<VCELog> pLog, shared_ptr<VCEStatus> pStatus, VCEInputInfo *pInfo, amf::AMFContextPtr pContext) {
    m_pVCELog = pLog;
    m_pStatus = pStatus;
    m_pContext = pContext;
    
    m_inputInfo = *pInfo;
    VCEInputAvsParam *pAvsParam = (VCEInputAvsParam *)pInfo->pPrivateParam;

    if (AMF_OK != load_avisynth()) {
        AddMessage(VCE_LOG_ERROR, _T("failed to load avisynth.dll.\n"));
        return AMF_FAIL;
    }

    if (NULL == (m_sAVSenv = m_sAvisynth.create_script_environment(AVISYNTH_INTERFACE_VERSION))) {
        AddMessage(VCE_LOG_ERROR, _T("failed to init avisynth enviroment.\n"));
        return AMF_FAIL;
    }
    std::string filename_char;
    if (0 == tchar_to_string(pAvsParam->srcFile, filename_char)) {
        AddMessage(VCE_LOG_ERROR, _T("failed to convert to ansi characters.\n"));
        return AMF_FAIL;
    }
    AVS_Value val_filename = avs_new_value_string(filename_char.c_str());
    AVS_Value val_res = m_sAvisynth.invoke(m_sAVSenv, "Import", val_filename, NULL);
    m_sAvisynth.release_value(val_filename);
    AddMessage(VCE_LOG_DEBUG, _T("opened avs file: \"%s\"\n"), char_to_tstring(filename_char).c_str());
    if (!avs_is_clip(val_res)) {
        AddMessage(VCE_LOG_ERROR, _T("invalid clip.\n"));
        if (avs_is_error(val_res)) {
            AddMessage(VCE_LOG_ERROR, char_to_tstring(avs_as_string(val_res)) + _T("\n"));
        }
        m_sAvisynth.release_value(val_res);
        return AMF_FAIL;
    }
    m_sAVSclip = m_sAvisynth.take_clip(val_res, m_sAVSenv);
    m_sAvisynth.release_value(val_res);

    if (NULL == (m_sAVSinfo = m_sAvisynth.get_video_info(m_sAVSclip))) {
        AddMessage(VCE_LOG_ERROR, _T("failed to get avs info.\n"));
        return AMF_FAIL;
    }

    if (!avs_has_video(m_sAVSinfo)) {
        AddMessage(VCE_LOG_ERROR, _T("avs has no video.\n"));
        return AMF_FAIL;
    }
    AddMessage(VCE_LOG_DEBUG, _T("found video from avs file, pixel type 0x%x.\n"), m_sAVSinfo->pixel_type);

    typedef struct CSPMap {
        int fmtID;
        VCE_CSP in, out;
    } CSPMap;

    static const std::vector<CSPMap> valid_csp_list = {
        { AVS_CS_YV12,  VCE_CSP_YV12, VCE_CSP_NV12 },
        { AVS_CS_I420,  VCE_CSP_YV12, VCE_CSP_NV12 },
        { AVS_CS_IYUV,  VCE_CSP_YV12, VCE_CSP_NV12 },
        { AVS_CS_YUY2,  VCE_CSP_YUY2, VCE_CSP_NV12 },
    };

    for (auto csp : valid_csp_list) {
        if (csp.fmtID == m_sAVSinfo->pixel_type) {
            m_pConvertCsp = get_convert_csp_func(csp.in, csp.out, false);
            break;
        }
    }

    if (nullptr == m_pConvertCsp) {
        AddMessage(VCE_LOG_ERROR, _T("invalid colorformat.\n"));
        return AMF_FAIL;
    }

    int fps_gcd = vce_gcd(m_sAVSinfo->fps_numerator, m_sAVSinfo->fps_denominator);
    m_inputInfo.srcWidth = m_sAVSinfo->width;
    m_inputInfo.srcHeight = m_sAVSinfo->height;
    m_inputInfo.fps.num = m_sAVSinfo->fps_numerator / fps_gcd;
    m_inputInfo.fps.den = m_sAVSinfo->fps_denominator / fps_gcd;
    m_inputInfo.frames = m_sAVSinfo->num_frames;

    tstring avisynth_version;
    AVS_Value val_version = m_sAvisynth.invoke(m_sAVSenv, "VersionNumber", avs_new_value_array(NULL, 0), NULL);
    if (avs_is_float(val_version)) {
        avisynth_version = strsprintf(_T("%.2f"), avs_as_float(val_version));
    }
    m_sAvisynth.release_value(val_version);

    tstring mes = strsprintf(_T("Avisynth %s %s->%s[%s], %dx%d%s, %d/%d fps"),
        avisynth_version.c_str(),
        VCE_CSP_NAMES[m_pConvertCsp->csp_from], VCE_CSP_NAMES[m_pConvertCsp->csp_to], get_simd_str(m_pConvertCsp->simd),
        m_inputInfo.srcWidth, m_inputInfo.srcHeight, m_inputInfo.interlaced ? _T("i") : _T("p"), m_inputInfo.fps.num, m_inputInfo.fps.den);

    AddMessage(VCE_LOG_DEBUG, _T("%s\n"), mes.c_str());
    m_strInputInfo += mes;
    return AMF_OK;
}

AMF_RESULT VCEInputAvs::Terminate() {
    AddMessage(VCE_LOG_DEBUG, _T("Closing...\n"));
    if (m_sAVSclip)
        m_sAvisynth.release_clip(m_sAVSclip);
    if (m_sAVSenv)
        m_sAvisynth.delete_script_environment(m_sAVSenv);

    release_avisynth();

    m_sAVSenv = nullptr;
    m_sAVSclip = nullptr;
    m_sAVSinfo = nullptr;

    m_pVCELog.reset();
    m_pStatus.reset();
    m_message.clear();
    m_pContext = nullptr;
    return AMF_OK;
}

AMF_RESULT VCEInputAvs::QueryOutput(amf::AMFData** ppData) {
    AMF_RESULT res = AMF_OK;
    amf::AMFSurfacePtr pSurface;
    res = m_pContext->AllocSurface(amf::AMF_MEMORY_HOST, m_inputInfo.format,
        m_inputInfo.srcWidth - m_inputInfo.crop.left - m_inputInfo.crop.right,
        m_inputInfo.srcHeight - m_inputInfo.crop.bottom - m_inputInfo.crop.up,
        &pSurface);
    if (res != AMF_OK) {
        AddMessage(VCE_LOG_ERROR, _T("AMFContext::AllocSurface(amf::AMF_MEMORY_HOST) failed.\n"));
        return res;
    }

    if (m_pStatus->m_inputFrames >= (uint32_t)m_inputInfo.frames) {
        return AMF_EOF;
    }

    AVS_VideoFrame *frame = m_sAvisynth.get_frame(m_sAVSclip, m_pStatus->m_inputFrames);
    if (frame == NULL) {
        return AMF_EOF;
    }
    const void *src_ptr[3] = { avs_get_read_ptr_p(frame, AVS_PLANAR_Y), avs_get_read_ptr_p(frame, AVS_PLANAR_U), avs_get_read_ptr_p(frame, AVS_PLANAR_V) };

    auto plane = pSurface->GetPlaneAt(0);
    int dst_stride = plane->GetHPitch();
    int dst_height = plane->GetVPitch();

    void *dst_ptr[2];
    dst_ptr[0] = (uint8_t *)plane->GetNative();
    dst_ptr[1] = (uint8_t *)dst_ptr[0] + dst_height * dst_stride;
    m_pConvertCsp->func[!!m_inputInfo.interlaced](dst_ptr, src_ptr, m_inputInfo.srcWidth, avs_get_pitch_p(frame, AVS_PLANAR_Y), avs_get_pitch_p(frame, AVS_PLANAR_U), dst_stride, m_inputInfo.srcHeight, dst_height, m_inputInfo.crop.c);
    m_pStatus->m_inputFrames++;

    m_sAvisynth.release_video_frame(frame);

    uint32_t tm = timeGetTime();
    if (tm - m_tmLastUpdate > 800) {
        m_tmLastUpdate = tm;
        m_pStatus->UpdateDisplay(tm, 0);
    }

    *ppData = pSurface.Detach();
    return AMF_OK;
}

#endif //ENABLE_AVISYNTH_READER