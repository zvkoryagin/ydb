#pragma once

#include <util/system/defaults.h>
#include <util/generic/strbuf.h>

#include <cstring>

enum MimeTypes {
    MIME_UNKNOWN = 0,
    MIME_TEXT = 1,
    MIME_HTML = 2,
    MIME_XHTMLXML = MIME_HTML,
    MIME_PDF = 3,
    MIME_RTF = 4,
    MIME_DOC = 5,
    MIME_MSWORD = MIME_DOC,
    MIME_MPEG = 6,
    MIME_XML = 7,
    MIME_RSS = MIME_XML,
    MIME_WML = 8,
    MIME_SWF = 9,
    MIME_FLASH = MIME_SWF,
    MIME_XLS = 10,
    MIME_EXCEL = MIME_XLS,
    MIME_PPT = 11,
    MIME_IMAGE_JPG = 12,
    MIME_IMAGE_PJPG = 13,
    MIME_IMAGE_PNG = 14,
    MIME_IMAGE_GIF = 15,
    MIME_DOCX = 16,
    MIME_ODT = 17,
    MIME_ODP = 18,
    MIME_ODS = 19,
    //MIME_XHTMLXML   = 20,
    MIME_IMAGE_BMP = 21,
    MIME_WAV = 22,
    MIME_ARCHIVE = 23,
    MIME_EXE = 24,
    MIME_ODG = 25,
    MIME_GZIP = 26,
    MIME_XLSX = 27,
    MIME_PPTX = 28,
    MIME_JAVASCRIPT = 29,
    MIME_EPUB = 30,
    MIME_TEX = 31,
    MIME_JSON = 32,
    MIME_APK = 33,
    MIME_CSS = 34,
    MIME_IMAGE_WEBP = 35,
    MIME_DJVU = 36,
    MIME_CHM = 37,
    MIME_FB2ZIP = 38,
    MIME_IMAGE_TIFF = 39,
    MIME_IMAGE_PNM = 40,
    MIME_IMAGE_SVG = 41,
    MIME_IMAGE_ICON = 42,
    MIME_WOFF = 43,
    MIME_WOFF2 = 44,
    MIME_TTF = 45,
    MIME_WEBMANIFEST = 46,
    MIME_CBOR = 47,
    MIME_CSV = 48,
    MIME_VIDEO_MP4 = 49,
    MIME_VIDEO_AVI = 50,
    MIME_MAX
};

extern const char* MimeNames[MIME_MAX];

const char* mimetypeByExt(const char* fname, const char* check_ext = nullptr);
MimeTypes mimeByStr(const char* mimeStr);
MimeTypes mimeByStr(const TStringBuf& mimeStr);
const char* strByMime(MimeTypes mime);

// autogenerated with GENERATE_ENUM_SERIALIZATION
const TString& ToString(MimeTypes x);