import glob
import os

Import("env")

project_dir = env.subst("$PROJECT_DIR")

patterns = [
    os.path.join(project_dir, ".pio", "libdeps", "*", "PNGdec", "**", "png.inl"),
]

found = []
for pat in patterns:
    found.extend(glob.glob(pat, recursive=True))

if not found:
    raise Exception("patch_png: png.inl not found under .pio/libdeps; cannot apply probes")

REPLACEMENTS = [
    (
        '#include "zlib.h"\n'
        '//\n'
        '// Convert 8-bit grayscale into RGB565\n'
        '//\n',
        '#include "zlib.h"\n'
        '// ---- diagnostic probes (temporary) ----\n'
        'volatile uint32_t g_pngInner = 0;\n'
        'volatile uint32_t g_pngOuter = 0;\n'
        'volatile uint32_t g_pngInflateCalls = 0;\n'
        '// ---- end probes ----\n'
        '//\n'
        '// Convert 8-bit grayscale into RGB565\n'
        '//\n',
    ),
    (
        "            case 0x49444154: //'IDAT' image data block\n"
        "                while (iLen) {\n"
        "                    if (iOffset >= iBytesRead) {\n"
        "                        // we ran out of data; get some more\n"
        "                        iBytesRead = (*pPage->pfnRead)(&pPage->PNGFile, pPage->ucFileBuf, (iLen > PNG_FILE_BUF_SIZE) ? PNG_FILE_BUF_SIZE : iLen);\n"
        "                        iFileOffset += iBytesRead;\n"
        "                        iOffset = 0;\n"
        "                    } else {\n",
        "            case 0x49444154: //'IDAT' image data block\n"
        "                while (iLen) {\n"
        "                    g_pngOuter++;\n"
        "                    if (iOffset >= iBytesRead) {\n"
        "                        // we ran out of data; get some more\n"
        "                        iBytesRead = (*pPage->pfnRead)(&pPage->PNGFile, pPage->ucFileBuf, (iLen > PNG_FILE_BUF_SIZE) ? PNG_FILE_BUF_SIZE : iLen);\n"
        "                        iFileOffset += iBytesRead;\n"
        "                        iOffset = 0;\n"
        '                        printf("[inf] readRAM got=%d iLen=%d\\r\\n", (int)iBytesRead, (int)iLen);\n'
        "                    } else {\n",
    ),
    (
        "                    err = 0;\n"
        "                    while (err == Z_OK) {\n"
        "                        if (d_stream.avail_out == 0) { // reset for next line\n"
        "                            d_stream.avail_out = pPage->iPitch+1;\n"
        "                            d_stream.next_out = pCurr;\n"
        "                        } // otherwise it could be a continuation of an unfinished line\n"
        "                        err = inflate(&d_stream, Z_NO_FLUSH, iOptions & PNG_CHECK_CRC);\n"
        "                        if ((err == Z_OK || err == Z_STREAM_END) && d_stream.avail_out == 0) {// successfully decoded line\n",
        "                    err = 0;\n"
        "                    while (err == Z_OK) {\n"
        "                        g_pngInner++;\n"
        "                        if (d_stream.avail_out == 0) { // reset for next line\n"
        "                            d_stream.avail_out = pPage->iPitch+1;\n"
        "                            d_stream.next_out = pCurr;\n"
        "                        } // otherwise it could be a continuation of an unfinished line\n"
        "                        g_pngInflateCalls++;\n"
        '                        printf("[inf] #%lu y=%d ain=%d aout=%d\\r\\n", (unsigned long)g_pngInflateCalls, (int)y, (int)d_stream.avail_in, (int)d_stream.avail_out);\n'
        "                        err = inflate(&d_stream, Z_NO_FLUSH, iOptions & PNG_CHECK_CRC);\n"
        '                        printf("[inf]   err=%d ain=%d aout=%d\\r\\n", (int)err, (int)d_stream.avail_in, (int)d_stream.avail_out);\n'
        "                        if ((err == Z_OK || err == Z_STREAM_END) && d_stream.avail_out == 0) {// successfully decoded line\n",
    ),
    (
        "                            DeFilter(pCurr, pPrev, pPage->iWidth, pPage->iPitch);\n",
        '                            printf("[inf] preDef y=%d\\r\\n", (int)y);\n'
        "                            DeFilter(pCurr, pPrev, pPage->iWidth, pPage->iPitch);\n"
        '                            printf("[inf] postDef y=%d\\r\\n", (int)y);\n',
    ),
    (
        "                                (*pPage->pfnDraw)(&pngd);\n",
        '                                printf("[inf] preDraw y=%d\\r\\n", (int)y);\n'
        "                                (*pPage->pfnDraw)(&pngd);\n"
        '                                printf("[inf] postDraw y=%d\\r\\n", (int)y);\n',
    ),
    (
        "                        // swap current and previous lines\n"
        "                        tmp = pCurr; pCurr = pPrev; pPrev = tmp;\n",
        "                        // swap current and previous lines\n"
        "                        tmp = pCurr; pCurr = pPrev; pPrev = tmp;\n"
        '                        printf("[inf] loopEnd y=%d\\r\\n", (int)y);\n',
    ),
]

for target in found:
    with open(target, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    for old, new in REPLACEMENTS:
        count = text.count(old)
        if count != 1:
            raise Exception(
                "patch_png: expected exactly 1 match for a probe anchor, found %d in %s"
                % (count, target)
            )
        text = text.replace(old, new)
    with open(target, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("patch_png: injected probes into %s" % target)
