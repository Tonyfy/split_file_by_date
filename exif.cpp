/*
 * Copyright (C) 2013 KLab Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef _MSC_VER
#include <windows.h>
#define vsnprintf _vsnprintf
#endif
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <memory.h>
#include <ctype.h>
#include "exif.hpp"

#pragma pack(2)

#define VERSION  "1.0.1"

// TIFF Header
typedef struct _tiff_Header {
    unsigned short byteOrder;
    unsigned short reserved;
    unsigned int Ifd0thOffset;
} TIFF_HEADER;

// APP1 Exif Segment Header
typedef struct _App1_Header {
    unsigned short marker;
    unsigned short length;
    char id[6]; // "Exif\0\0"
    TIFF_HEADER tiff;
} APP1_HEADER;

// tag field in IFD
typedef struct {
    unsigned short tag;
    unsigned short type;
    unsigned int count;
    unsigned int offset;
} IFD_TAG;

// tag node - internal use
typedef struct _tagNode TagNode;
struct _tagNode {
    unsigned short tagId;
    unsigned short type;
    unsigned int count;
    unsigned int *numData;
    unsigned char *byteData;
    unsigned short error;
    TagNode *prev;
    TagNode *next;
};

// IFD table - internal use
typedef struct _ifdTable IfdTable;
struct _ifdTable {
    IFD_TYPE ifdType;
    unsigned short tagCount;
    TagNode *tags;
    unsigned int nextIfdOffset;
    unsigned short offset;
    unsigned short length;
    unsigned char *p;
};

static int init(FILE*);
static int systemIsLittleEndian();
static int dataIsLittleEndian();
static void freeIfdTable(void*);
static void *parseIFD(FILE*, unsigned int, IFD_TYPE);
static TagNode *getTagNodePtrFromIfd(IfdTable*, unsigned short);
static TagNode *duplicateTagNode(TagNode*);
static void freeTagNode(void*);
static char *getTagName(int, unsigned short);
static int countIfdTableOnIfdTableArray(void **ifdTableArray);
static IfdTable *getIfdTableFromIfdTableArray(void **ifdTableArray, IFD_TYPE ifdType);
static void *createIfdTable(IFD_TYPE IfdType, unsigned short tagCount, unsigned int nextOfs);
static void *addTagNodeToIfd(void *pIfd, unsigned short tagId, unsigned short type,
                      unsigned int count, unsigned int *numData,unsigned char *byteData);
static int writeExifSegment(FILE *fp, void **ifdTableArray);
static int removeTagOnIfd(void *pIfd, unsigned short tagId);
static int fixLengthAndOffsetInIfdTables(void **ifdTableArray);
static int setSingleNumDataToTag(TagNode *tag, unsigned int value);
static int getApp1StartOffset(FILE *fp, const char *App1IDString,
                              size_t App1IDStringLength, int *pDQTOffset);
static unsigned short swab16(unsigned short us);
static void PRINTF(char **ms, const char *fmt, ...);
static int _dumpIfdTable(void *pIfd, char **p);

static int Verbose = 0;
static int App1StartOffset = -1;
static int JpegDQTOffset = -1;
static APP1_HEADER App1Header;

// public funtions

/**
 * setVerbose()
 *
 * Verbose output on/off
 *
 * parameters
 *  [in] v : 1=on  0=off
 */
void setVerbose(int v)
{
    Verbose = v;
}

/**
 * removeExifSegmentFromJPEGFile()
 *
 * Remove the Exif segment from a JPEG file
 *
 * parameters
 *  [in] inJPEGFileName : original JPEG file
 *  [in] outJPGEFileName : output JPEG file
 *
 * return
 *   1: OK
 *   0: the Exif segment is not found
 *  -n: error
 *      ERR_READ_FILE
 *      ERR_WRITE_FILE
 *      ERR_INVALID_JPEG
 *      ERR_INVALID_APP1HEADER
 */
int removeExifSegmentFromJPEGFile(const char *inJPEGFileName,
                                  const char *outJPGEFileName)
{
    int ofs;
    int i, sts = 1;
    size_t readLen, writeLen;
    unsigned char buf[8192], *p;
    FILE *fpr = NULL, *fpw = NULL;

    fpr = fopen(inJPEGFileName, "rb");
    if (!fpr) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fpr);
    if (sts <= 0) {
        goto DONE;
    }
    fpw = fopen(outJPGEFileName, "wb");
    if (!fpw) {
        sts = ERR_WRITE_FILE;
        goto DONE;
    }
    // copy the data in front of the Exif segment
    rewind(fpr);
    p = buf;
    if (App1StartOffset > sizeof(buf)) {
        // allocate new buffer if needed
        p = (unsigned char*)malloc(App1StartOffset);
    }
    if (!p) {
        for (i = 0; i < App1StartOffset; i++) {
            fread(buf, 1, sizeof(char), fpr);
            fwrite(buf, 1, sizeof(char), fpw);
        }
    } else {
        if (fread(p, 1, App1StartOffset, fpr) < (size_t)App1StartOffset) {
            sts = ERR_READ_FILE;
            goto DONE;
        }
        if (fwrite(p, 1, App1StartOffset, fpw) < (size_t)App1StartOffset) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
        if (p != &buf[0]) {
            free(p);
        }
    }
    // seek to the end of the Exif segment
    ofs = App1StartOffset + sizeof(App1Header.marker) + App1Header.length;
    if (fseek(fpr, ofs, SEEK_SET) != 0) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    // read & write
    for (;;) {
        readLen = fread(buf, 1, sizeof(buf), fpr);
        if (readLen <= 0) {
            break;
        }
        writeLen = fwrite(buf, 1, readLen, fpw);
        if (writeLen != readLen) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
    }
DONE:
    if (fpw) {
        fclose(fpw);
    }
    if (fpr) {
        fclose(fpr);
    }
    return sts;
}

/**
 * createIfdTableArray()
 *
 * Parse the JPEG header and create the pointer array of the IFD tables
 *
 * parameters
 *  [in] JPEGFileName : target JPEG file
 *  [out] result : result status value 
 *   n: number of IFD tables
 *   0: the Exif segment is not found
 *  -n: error
 *      ERR_READ_FILE
 *      ERR_INVALID_JPEG
 *      ERR_INVALID_APP1HEADER
 *      ERR_INVALID_IFD
 *
 * return
 *   NULL: error or no Exif segment
 *  !NULL: pointer array of the IFD tables
 */
void **createIfdTableArray(const char *JPEGFileName, int *result)
{
    #define FMT_ERR "critical error in %s IFD\n"

    int i, sts = 1, ifdCount = 0;
    unsigned int ifdOffset;
    FILE *fp = NULL;
    TagNode *tag;
    void **ppIfdArray = NULL;
    void *ifdArray[32];
    IfdTable *ifd_0th, *ifd_exif, *ifd_gps, *ifd_io, *ifd_1st;

    ifd_0th = ifd_exif = ifd_gps = ifd_io = ifd_1st = NULL;
    memset(ifdArray, 0, sizeof(ifdArray));

    fp = fopen(JPEGFileName, "rb");
    if (!fp) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fp);
    if (sts <= 0) {
        goto DONE;
    }
    if (Verbose) {
        printf("system: %s-endian\n  data: %s-endian\n", 
            systemIsLittleEndian() ? "little" : "big",
            dataIsLittleEndian() ? "little" : "big");
    }

    // for 0th IFD
	ifd_0th = (IfdTable*)parseIFD(fp, App1Header.tiff.Ifd0thOffset, IFD_0TH);
    if (!ifd_0th) {
        if (Verbose) {
            printf(FMT_ERR, "0th");
        }
        sts = ERR_INVALID_IFD;
        goto DONE; // non-continuable
    }
    ifdArray[ifdCount++] = ifd_0th;

    // for Exif IFD 
    tag = getTagNodePtrFromIfd(ifd_0th, TAG_ExifIFDPointer);
    if (tag && !tag->error) {
        ifdOffset = tag->numData[0];
        if (ifdOffset != 0) {
			ifd_exif = (IfdTable*)parseIFD(fp, ifdOffset, IFD_EXIF);
            if (ifd_exif) {
                ifdArray[ifdCount++] = ifd_exif;
                // for InteroperabilityIFDPointer IFD
                tag = getTagNodePtrFromIfd(ifd_exif, TAG_InteroperabilityIFDPointer);
                if (tag && !tag->error) {
                    ifdOffset = tag->numData[0];
                    if (ifdOffset != 0) {
						ifd_io = (IfdTable*)parseIFD(fp, ifdOffset, IFD_IO);
                        if (ifd_io) {
                            ifdArray[ifdCount++] = ifd_io;
                        } else {
                            if (Verbose) {
                                printf(FMT_ERR, "Interoperability");
                            }
                            sts = ERR_INVALID_IFD;
                        }
                    }
                }
            } else {
                if (Verbose) {
                    printf(FMT_ERR, "Exif");
                }
                sts = ERR_INVALID_IFD;
            }
        }
    }

    // for GPS IFD
    tag = getTagNodePtrFromIfd(ifd_0th, TAG_GPSInfoIFDPointer);
    if (tag && !tag->error) {
        ifdOffset = tag->numData[0];
        if (ifdOffset != 0) {
			ifd_gps = (IfdTable*)parseIFD(fp, ifdOffset, IFD_GPS);
            if (ifd_gps) {
                ifdArray[ifdCount++] = ifd_gps;
            } else {
                if (Verbose) {
                    printf(FMT_ERR, "GPS");
                }
                sts = ERR_INVALID_IFD;
            }
        }
    }

    // for 1st IFD
    ifdOffset = ifd_0th->nextIfdOffset;
    if (ifdOffset != 0) {
		ifd_1st = (IfdTable*)parseIFD(fp, ifdOffset, IFD_1ST);
        if (ifd_1st) {
            ifdArray[ifdCount++] = ifd_1st;
        } else {
            if (Verbose) {
                printf(FMT_ERR, "1st");
            }
            sts = ERR_INVALID_IFD;
        }
    }

DONE:
    *result = (sts <= 0) ? sts : ifdCount;
    if (ifdCount > 0) {
        // +1 extra NULL element to the array 
        ppIfdArray = (void**)malloc(sizeof(void*)*(ifdCount+1));
        memset(ppIfdArray, 0, sizeof(void*)*(ifdCount+1));
        for (i = 0; ifdArray[i] != NULL; i++) {
            ppIfdArray[i] = ifdArray[i];
        }
    }
    if (fp) {
        fclose(fp);
    }
    return ppIfdArray;
}

/**
 * freeIfdTableArray()
 *
 * Free the pointer array of the IFD tables
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 */
void freeIfdTableArray(void **ifdArray)
{
    int i;
    for (i = 0; ifdArray[i] != NULL; i++) {
        freeIfdTable(ifdArray[i]);
    }
    free(ifdArray);
}

/**
 * getIfdType()
 *
 * Returns the type of the IFD
 *
 * parameters
 *  [in] ifd: target IFD
 *
 * return
 *  IFD TYPE value
 */
IFD_TYPE getIfdType(void *pIfd)
{
    IfdTable *ifd = (IfdTable*)pIfd;
    if (!ifd) {
        return IFD_UNKNOWN;
    }
    return ifd->ifdType;
}

/**
 * dumpIfdTable()
 *
 * Dump the IFD table
 *
 * parameters
 *  [in] ifd: target IFD
 */

int dumpIfdTable(void *pIfd)
{
    return _dumpIfdTable(pIfd, NULL);
}

void getIfdTableDump(void *pIfd, char **pp)
{
    if (pp) {
        *pp = NULL;
    }
    _dumpIfdTable(pIfd, pp);
}

static int _dumpIfdTable(void *pIfd, char **p)
{
    int i;
    IfdTable *ifd;
    TagNode *tag;
    char tagName[512];
    int cnt = 0;
    unsigned int count;

    if (!pIfd) {
        return 0;
    }
    ifd = (IfdTable*)pIfd;

    PRINTF(p, "\n{%s IFD}",
        (ifd->ifdType == IFD_0TH)  ? "0TH" :
        (ifd->ifdType == IFD_1ST)  ? "1ST" :
        (ifd->ifdType == IFD_EXIF) ? "EXIF" :
        (ifd->ifdType == IFD_GPS)  ? "GPS" :
        (ifd->ifdType == IFD_IO)   ? "Interoperability" : "");

    if (Verbose) {
        PRINTF(p, " tags=%u\n", ifd->tagCount);
    } else {
        PRINTF(p, "\n");
    }

    tag = ifd->tags;
    while (tag) {
        if (Verbose) {
            PRINTF(p, "tag[%02d] 0x%04X %s\n",
                cnt++, tag->tagId, getTagName(ifd->ifdType, tag->tagId));
            PRINTF(p, "\ttype=%u count=%u ", tag->type, tag->count);
            PRINTF(p, "val=");
        } else {
            strcpy(tagName, getTagName(ifd->ifdType, tag->tagId));
            PRINTF(p, " - %s: ", (strlen(tagName) > 0) ? tagName : "(unknown)");
			if (!strcmp(tagName, "Orientation"))
			{
				return (unsigned short)(tag->numData[0]);
			}
			
        }
		tag = tag->next;
    }

}

/**
 * dumpIfdTableArray()
 *
 * Dump the array of the IFD tables
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 */
void dumpIfdTableArray(void **ifdArray)
{
    int i;
    if (ifdArray) {
        for (i = 0; ifdArray[i] != NULL; i++) {
            dumpIfdTable(ifdArray[i]);
        }
    }
}

/**
 * getTagInfo()
 *
 * Get the TagNodeInfo that matches the IFD_TYPE & TagId
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 *  [in] ifdType : target IFD TYPE
 *  [in] tagId : target tag ID
 *
 * return
 *   NULL: tag is not found
 *  !NULL: address of the TagNodeInfo structure
 */
TagNodeInfo *getTagInfo(void **ifdArray,
                       IFD_TYPE ifdType,
                       unsigned short tagId)
{
    int i;
    if (!ifdArray) {
        return NULL;
    }
    for (i = 0; ifdArray[i] != NULL; i++) {
        if (getIfdType(ifdArray[i]) == ifdType) {
			void *targetTag = getTagNodePtrFromIfd((IfdTable*)ifdArray[i], tagId);
            if (!targetTag) {
                return NULL;
            }
			return (TagNodeInfo*)duplicateTagNode((TagNode *)targetTag);
        }
    }
    return NULL;
}

/**
 * getTagInfoFromIfd()
 *
 * Get the TagNodeInfo that matches the TagId
 *
 * parameters
 *  [in] ifd : target IFD table
 *  [in] tagId : target tag ID
 *
 * return
 *  NULL: tag is not found
 *  !NULL: address of the TagNodeInfo structure
 */
TagNodeInfo *getTagInfoFromIfd(void *ifd,
                               unsigned short tagId)
{
    if (!ifd) {
        return NULL;
    }
	return (TagNodeInfo*)getTagNodePtrFromIfd((IfdTable*)ifd, tagId);
}

/**
 * freeTagInfo()
 *
 * Free the TagNodeInfo allocated by getTagInfo() or getTagInfoFromIfd()
 *
 * parameters
 *  [in] tag : target TagNodeInfo
 */
void freeTagInfo(void *tag)
{
    freeTagNode(tag);
}

/**
 * queryTagNodeIsExist()
 *
 * Query if the specified tag node is exist in the IFD tables
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [in] tagId : target tag ID
 *
 * return
 *  0: not exist
 *  1: exist
 */
int queryTagNodeIsExist(void **ifdTableArray,
                        IFD_TYPE ifdType,
                        unsigned short tagId)
{
    IfdTable *ifd;
    TagNode *tag;
    if (!ifdTableArray) {
        return 0;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, ifdType);
    if (!ifd) {
        return 0;
    }
    tag = getTagNodePtrFromIfd(ifd, tagId);
    if (!tag) {
        return 0;
    }
    return 1;
}

/**
 * createTagInfo()
 *
 * Create new TagNodeInfo block
 *
 * parameters
 *  [in] tagId: id of the tag
 *  [in] type: type of the tag
 *  [in] count: data count of the tag
 *  [out] pResult : error status
 *   0: OK
 *  -n: error
 *      ERR_INVALID_TYPE
 *      ERR_INVALID_COUNT
 *      ERR_MEMALLOC
 *
 * return
 *  NULL: error
 * !NULL: address of the newly created TagNodeInfo
 */
TagNodeInfo *createTagInfo(unsigned short tagId,
                           unsigned short type,
                           unsigned int count,
                           int *pResult)
{
    TagNode *tag;
    if (type < TYPE_BYTE || type > TYPE_SRATIONAL) {
        if (pResult) {
            *pResult = ERR_INVALID_TYPE;
        }
        return NULL;
    }
    if (count <= 0) {
        if (pResult) {
            *pResult = ERR_INVALID_COUNT;
        }
        return NULL;
    }
    tag = (TagNode*)malloc(sizeof(TagNode));
    if (!tag) {
        if (pResult) {
            *pResult = ERR_MEMALLOC;
        }
        return NULL;
    }
    memset(tag, 0, sizeof(TagNode));
    tag->tagId = tagId;
    tag->type = type;
    tag->count = count;

    if (type == TYPE_ASCII || type == TYPE_UNDEFINED) {
        tag->byteData = (unsigned char*)malloc(count*sizeof(char));
    }
    else if (type == TYPE_BYTE   ||
             type == TYPE_SBYTE  ||
             type == TYPE_SHORT  ||
             type == TYPE_LONG   ||
             type == TYPE_SSHORT ||
             type == TYPE_SLONG) {
        tag->numData = (unsigned int*)malloc(count*sizeof(int));
    }
    else if (type == TYPE_RATIONAL ||
             type == TYPE_SRATIONAL) {
        tag->numData = (unsigned int*)malloc(count*sizeof(int)*2);
    }
    if (pResult) {
        *pResult = 0;
    }
    return (TagNodeInfo*)tag;
}

/**
 * removeIfdTableFromIfdTableArray()
 *
 * Remove the IFD table from the ifdTableArray
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *
 * return
 *  n: number of the removed IFD tables
 */
int removeIfdTableFromIfdTableArray(void **ifdTableArray, IFD_TYPE ifdType)
{
    int i, num = 0, ret = 0;
    if (!ifdTableArray) {
        return 0;
    }
    // count IFD tables
    num = countIfdTableOnIfdTableArray(ifdTableArray);
    for (;;) { // possibility of multiple entries
        for (i = 0; i < num; i++) {
			IfdTable *ifd = (IfdTable *)ifdTableArray[i];
            if (ifd->ifdType == ifdType) {
                freeIfdTable(ifd);
                ifdTableArray[i] = NULL;
                ret++;
                break;
            }
        }
        if (i == num) {
            break; // no more found
        }
        // left justify the array
        memcpy(&ifdTableArray[i], &ifdTableArray[i+1], (num-i) * sizeof(void*));
        num--;
    }
    return ret;
}

/**
 * insertIfdTableToIfdTableArray()
 *
 * Insert new IFD table to the ifdTableArray
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [out] pResult : error status
 *   0: OK
 *  -n: error
 *      ERR_ALREADY_EXIST
 *      ERR_MEMALLOC
 *
 * return
 *  NULL: error
 * !NULL: address of the newly created ifdTableArray
 *
 * note
 * This function frees old ifdTableArray if is not NULL.
 */
void **insertIfdTableToIfdTableArray(void **ifdTableArray,
                                     IFD_TYPE ifdType,
                                     int *pResult)
{
    void *newIfd;
    void **newIfdTableArray;
    int num = 0;
    if (!ifdTableArray) {
        num = 0;
    } else {
        num = countIfdTableOnIfdTableArray(ifdTableArray);
    }
    if (num > 0 && getIfdTableFromIfdTableArray(ifdTableArray, ifdType) != NULL) {
        if (pResult) {
            *pResult = ERR_ALREADY_EXIST;
        }
        return NULL;
    }
    // create the new IFD table
    newIfd = createIfdTable(ifdType, 0, 0);
    if (!newIfd) {
        if (pResult) {
            *pResult = ERR_MEMALLOC;
        }
        return NULL;
    }
    // copy existing IFD tables to the new array
    newIfdTableArray = (void**)malloc(sizeof(void*)*(num+2));
    if (!newIfdTableArray) {
        if (pResult) {
            *pResult = ERR_MEMALLOC;
        }
        free(newIfd);
        return NULL;
    }
    memset(newIfdTableArray, 0, sizeof(void*)*(num+2));
    if (num > 0) {
        memcpy(newIfdTableArray, ifdTableArray, num * sizeof(void*));
    }
    // add the new IFD table
    newIfdTableArray[num] = newIfd;
    if (ifdTableArray) {
        free(ifdTableArray); // free the old array
    }
    if (pResult) {
        *pResult = 0;
    }
    return newIfdTableArray;
}

/**
 * removeTagNodeFromIfdTableArray()
 *
 * Remove the specified tag node from the IFD table
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [in] tagId : target tag ID
 *
 * return
 *  n: number of the removed tags
 */
int removeTagNodeFromIfdTableArray(void **ifdTableArray,
                             IFD_TYPE ifdType,
                             unsigned short tagId)
{
    IfdTable *ifd = getIfdTableFromIfdTableArray(ifdTableArray, ifdType);
    if (!ifd) {
        return 0;
    }
    return removeTagOnIfd(ifd, tagId);
}

/**
 * insertTagNodeToIfdTableArray()
 *
 * Insert the specified tag node to the IFD table
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [in] tagNodeInfo: address of the TagNodeInfo
 *
 * note
 * This function uses the copy of the specified tag data.
 * The caller must free it after this function returns.
 *
 * return
 *  0: OK
 *  ERR_INVALID_POINTER:
 *  ERR_NOT_EXIST:
 *  ERR_ALREADY_EXIST:
 *  ERR_UNKNOWN:
 */
int insertTagNodeToIfdTableArray(void **ifdTableArray,
                             IFD_TYPE ifdType,
                             TagNodeInfo *tagNodeInfo)
{
    IfdTable *ifd;
    if (!ifdTableArray) {
        return ERR_INVALID_POINTER;
    }
    if (!tagNodeInfo) {
        return ERR_INVALID_POINTER;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, ifdType);
    if (!ifd) {
        return ERR_NOT_EXIST;
    }
    // already exists the same type entry
    if (getTagNodePtrFromIfd(ifd, tagNodeInfo->tagId) != NULL) {
        return ERR_ALREADY_EXIST;
    }
    // add to the IFD table
    if (!addTagNodeToIfd(ifd, 
                    tagNodeInfo->tagId,
                    tagNodeInfo->type,
                    tagNodeInfo->count,
                    tagNodeInfo->numData,
                    tagNodeInfo->byteData)) {
        return ERR_UNKNOWN;
    }
    ifd->tagCount++;
    return 0;
}

/**
 * getThumbnailDataOnIfdTableArray()
 *
 * Get a copy of the thumbnail data from the 1st IFD table
 *
 * parameters
 *  [in] ifdTableArray : address of the IFD tables array
 *  [out] pLength : returns the length of the thumbnail data
 *  [out] pResult : error status
 *   0: OK
 *  -n: error
 *      ERR_INVALID_POINTER
 *      ERR_MEMALLOC
 *      ERR_NOT_EXIST
 *
 * return
 *  NULL: error
 * !NULL: the thumbnail data
 *
 * note
 * This function returns the copy of the thumbnail data.
 * The caller must free it.
 */
unsigned char *getThumbnailDataOnIfdTableArray(void **ifdTableArray,
                                               unsigned int *pLength,
                                               int *pResult)
{
    IfdTable *ifd;
    TagNode *tag;
    unsigned int len;
    unsigned char *retp;
    if (!ifdTableArray || !pLength) {
        if (pResult) {
            *pResult = ERR_INVALID_POINTER;
        }
        return NULL;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
    if (!ifd || !ifd->p) {
        if (pResult) {
            *pResult = ERR_NOT_EXIST;
        }
        return NULL;
    }
    tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
    if (!tag || tag->error) {
        if (pResult) {
            *pResult = ERR_NOT_EXIST;
        }
        return NULL;
    }
    len = tag->numData[0];
    if (len <= 0) {
        if (pResult) {
            *pResult = ERR_NOT_EXIST;
        }
        return NULL;
    }
    retp= (unsigned char*)malloc(len);
    if (!retp) {
        if (pResult) {
            *pResult = ERR_MEMALLOC;
        }
        return NULL;
    }
    memcpy(retp, ifd->p, len);
    *pLength = len;
    if (pResult) {
        *pResult = 0;
    }
    return retp;
}

/**
 * setThumbnailDataOnIfdTableArray()
 *
 * Set or update the thumbnail data to the 1st IFD table
 *
 * parameters
 *  [in] ifdTableArray : address of the IFD tables array
 *  [in] pData : thumbnail data
 *  [in] length : thumbnail data length
 *
 * note
 * This function creates the copy of the specified data.
 * The caller must free it after this function returns.
 *
 * return
 *   0: OK
 *  -n: error
 *      ERR_INVALID_POINTER
 *      ERR_MEMALLOC
 *      ERR_UNKNOWN
 */
int setThumbnailDataOnIfdTableArray(void **ifdTableArray,
                                    unsigned char *pData,
                                    unsigned int length)
{
    IfdTable *ifd;
    TagNode *tag;
    unsigned int zero = 0;
    if (!ifdTableArray || !pData || length <= 0) {
        return ERR_INVALID_POINTER;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
    if (!ifd) {
        return ERR_NOT_EXIST;
    }
    if (ifd->p) {
        free(ifd->p);
    }
    // set thumbnail length;
    tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
    if (tag) {
        setSingleNumDataToTag(tag, length);
    } else {
        if (!addTagNodeToIfd(ifd, TAG_JPEGInterchangeFormatLength,
                            TYPE_LONG, 1, &length, NULL)) {
            return ERR_UNKNOWN;
        }
    }
    tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormat);
    if (tag) {
        setSingleNumDataToTag(tag, zero);
    } else {
        // add thumbnail offset tag if not exist
        addTagNodeToIfd(ifd, TAG_JPEGInterchangeFormat,
                            TYPE_LONG, 1, &zero, NULL);
    }
    ifd->p = (unsigned char*)malloc(length);
    if (!ifd->p) {
        return ERR_MEMALLOC;
    }
    memcpy(ifd->p, pData, length);
    return 0;
}

/**
 * updateExifSegmentInJPEGFile()
 *
 * Update the Exif segment in a JPEG file
 *
 * parameters
 *  [in] inJPEGFileName : original JPEG file
 *  [in] outJPGEFileName : output JPEG file
 *  [in] ifdTableArray : address of the IFD tables array
 *
 * return
 *   1: OK
 *  -n: error
 *      ERR_READ_FILE
 *      ERR_WRITE_FILE
 *      ERR_INVALID_JPEG
 *      ERR_INVALID_APP1HEADER
 *      ERR_INVALID_POINTER
 *      ERROR_UNKNOWN:
 */
int updateExifSegmentInJPEGFile(const char *inJPEGFileName,
                                const char *outJPGEFileName,
                                void **ifdTableArray)
{
    int ofs;
    int i, sts = 1, hasExifSegment;
    size_t readLen, writeLen;
    unsigned char buf[8192], *p;
    FILE *fpr = NULL, *fpw = NULL;

    // refresh the length and offset variables in the IFD table
    sts = fixLengthAndOffsetInIfdTables(ifdTableArray);
    if (sts != 0) {
        goto DONE;
    }
    fpr = fopen(inJPEGFileName, "rb");
    if (!fpr) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fpr);
    if (sts < 0) {
        goto DONE;
    }
    if (sts == 0) {
        hasExifSegment = 0;
        ofs = JpegDQTOffset;
    } else {
        hasExifSegment = 1;
        ofs = App1StartOffset;
    }
    fpw = fopen(outJPGEFileName, "wb");
    if (!fpw) {
        sts = ERR_WRITE_FILE;
        goto DONE;
    }
    // copy the data in front of the Exif segment
    rewind(fpr);
    p = buf;
    if (ofs > sizeof(buf)) {
        // allocate new buffer if needed
        p = (unsigned char*)malloc(ofs);
    }
    if (!p) {
        for (i = 0; i < ofs; i++) {
            fread(buf, 1, sizeof(char), fpr);
            fwrite(buf, 1, sizeof(char), fpw);
        }
    } else {
        if (fread(p, 1, ofs, fpr) < (size_t)ofs) {
            sts = ERR_READ_FILE;
            goto DONE;
        }
        if (fwrite(p, 1, ofs, fpw) < (size_t)ofs) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
        if (p != &buf[0]) {
            free(p);
        }
    }
    // write new Exif segment
    sts = writeExifSegment(fpw, ifdTableArray);
    if (sts != 0) {
        goto DONE;
    }
    sts = 1;
    if (hasExifSegment) {
        // seek to the end of the Exif segment
        ofs = App1StartOffset + sizeof(App1Header.marker) + App1Header.length;
        if (fseek(fpr, ofs, SEEK_SET) != 0) {
            sts = ERR_READ_FILE;
            goto DONE;
        }
    }
    // read & write
    for (;;) {
        readLen = fread(buf, 1, sizeof(buf), fpr);
        if (readLen <= 0) {
            break;
        }
        writeLen = fwrite(buf, 1, readLen, fpw);
        if (writeLen != readLen) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
    }
DONE:
    if (fpw) {
        fclose(fpw);
    }
    if (fpr) {
        fclose(fpr);
    }
    return sts;
}

/**
 * removeAdobeMetadataSegmentFromJPEGFile()
 *
 * Remove Adobe's XMP metadata segment from a JPEG file
 *
 * parameters
 *  [in] inJPEGFileName : original JPEG file
 *  [in] outJPGEFileName : output JPEG file
 *
 * return
 *   1: OK
 *   0: Adobe's metadata segment is not found
 *  -n: error
 *      ERR_READ_FILE
 *      ERR_WRITE_FILE
 *      ERR_INVALID_JPEG
 *      ERR_INVALID_APP1HEADER
 */
int removeAdobeMetadataSegmentFromJPEGFile(const char *inJPEGFileName,
                                           const char *outJPGEFileName)
{
#define ADOBE_METADATA_ID     "http://ns.adobe.com/xap/"
#define ADOBE_METADATA_ID_LEN 24

    typedef struct _SegmentHeader {
        unsigned short marker;
        unsigned short length;
    } SEGMENT_HEADER;

    SEGMENT_HEADER hdr;
    int i, sts = 1;
    size_t readLen, writeLen;
    unsigned int ofs;
    unsigned char buf[8192], *p;
    FILE *fpr = NULL, *fpw = NULL;

    fpr = fopen(inJPEGFileName, "rb");
    if (!fpr) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    sts = getApp1StartOffset(fpr, ADOBE_METADATA_ID, ADOBE_METADATA_ID_LEN, NULL);
    if (sts <= 0) { // target segment is not exist or something error
        goto DONE;
    }
    ofs = sts;
    sts = 1;
    fpw = fopen(outJPGEFileName, "wb");
    if (!fpw) {
        sts = ERR_WRITE_FILE;
        goto DONE;
    }
    // copy the data in front of the App1 segment
    rewind(fpr);
    p = buf;
    if (ofs > sizeof(buf)) {
        // allocate new buffer if needed
        p = (unsigned char*)malloc(ofs);
    }
    if (!p) {
        for (i = 0; i < (int)ofs; i++) {
            fread(buf, 1, sizeof(char), fpr);
            fwrite(buf, 1, sizeof(char), fpw);
        }
    } else {
        if (fread(p, 1, ofs, fpr) < (size_t)ofs) {
            sts = ERR_READ_FILE;
            goto DONE;
        }
        if (fwrite(p, 1, ofs, fpw) < (size_t)ofs) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
        if (p != &buf[0]) {
            free(p);
        }
    }
    if (fread(&hdr, 1, sizeof(SEGMENT_HEADER), fpr) != sizeof(SEGMENT_HEADER)) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    if (systemIsLittleEndian()) {
        // the segment length value is always in big-endian order
        hdr.length = swab16(hdr.length);
    }
    // seek to the end of the App1 segment
    if (fseek(fpr, hdr.length - sizeof(hdr.length), SEEK_CUR) != 0) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    // read & write
    for (;;) {
        readLen = fread(buf, 1, sizeof(buf), fpr);
        if (readLen <= 0) {
            break;
        }
        writeLen = fwrite(buf, 1, readLen, fpw);
        if (writeLen != readLen) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
    }
DONE:
    if (fpw) {
        fclose(fpw);
    }
    if (fpr) {
        fclose(fpr);
    }
    return sts;
}

// private functions

static int dataIsLittleEndian()
{
    return (App1Header.tiff.byteOrder == 0x4949) ? 1 : 0;
}

static int systemIsLittleEndian()
{
    static int i = 1;
    return (int)(*(char*)&i);
}

static unsigned short swab16(unsigned short us)
{
    return (us << 8) | ((us >> 8) & 0x00FF);
}

static unsigned int swab32(unsigned int ui)
{
    return
    ((ui << 24) & 0xFF000000) | ((ui << 8)  & 0x00FF0000) |
    ((ui >> 8)  & 0x0000FF00) | ((ui >> 24) & 0x000000FF);
}

static unsigned short fix_short(unsigned short us)
{
    return (dataIsLittleEndian() !=
        systemIsLittleEndian()) ? swab16(us) : us;
}

static unsigned int fix_int(unsigned int ui)
{
    return (dataIsLittleEndian() !=
        systemIsLittleEndian()) ? swab32(ui) : ui;
}

static int seekToRelativeOffset(FILE *fp, unsigned int ofs)
{
    static int start = offsetof(APP1_HEADER, tiff);
    return fseek(fp, (App1StartOffset + start) + ofs, SEEK_SET);
}

static char *getTagName(int ifdType, unsigned short tagId)
{
    static char tagName[128];
    if (ifdType == IFD_0TH || ifdType == IFD_1ST || ifdType == IFD_EXIF) {
        strcpy(tagName,
            (tagId == 0x0100) ? "ImageWidth" :
            (tagId == 0x0101) ? "ImageLength" :
            (tagId == 0x0102) ? "BitsPerSample" :
            (tagId == 0x0103) ? "Compression" :
            (tagId == 0x0106) ? "PhotometricInterpretation" :
            (tagId == 0x0112) ? "Orientation" :
            (tagId == 0x0115) ? "SamplesPerPixel" :
            (tagId == 0x011C) ? "PlanarConfiguration" :
            (tagId == 0x0212) ? "YCbCrSubSampling" :
            (tagId == 0x0213) ? "YCbCrPositioning" :
            (tagId == 0x011A) ? "XResolution" :
            (tagId == 0x011B) ? "YResolution" :
            (tagId == 0x0128) ? "ResolutionUnit" :

            (tagId == 0x0111) ? "StripOffsets" :
            (tagId == 0x0116) ? "RowsPerStrip" :
            (tagId == 0x0117) ? "StripByteCounts" :
            (tagId == 0x0201) ? "JPEGInterchangeFormat" :
            (tagId == 0x0202) ? "JPEGInterchangeFormatLength" :

            (tagId == 0x012D) ? "TransferFunction" :
            (tagId == 0x013E) ? "WhitePoint" :
            (tagId == 0x013F) ? "PrimaryChromaticities" :
            (tagId == 0x0211) ? "YCbCrCoefficients" :
            (tagId == 0x0214) ? "ReferenceBlackWhite" :

            (tagId == 0x0132) ? "DateTime" :
            (tagId == 0x010E) ? "ImageDescription" :
            (tagId == 0x010F) ? "Make" :
            (tagId == 0x0110) ? "Model" :
            (tagId == 0x0131) ? "Software" :
            (tagId == 0x013B) ? "Artist" :
            (tagId == 0x8298) ? "Copyright" :
            (tagId == 0x8769) ? "ExifIFDPointer" :
            (tagId == 0x8825) ? "GPSInfoIFDPointer":
            (tagId == 0xA005) ? "InteroperabilityIFDPointer" :

            (tagId == 0x4746) ? "Rating" :

            (tagId == 0x9000) ? "ExifVersion" :
            (tagId == 0xA000) ? "FlashPixVersion" :

            (tagId == 0xA001) ? "ColorSpace" :

            (tagId == 0x9101) ? "ComponentsConfiguration" :
            (tagId == 0x9102) ? "CompressedBitsPerPixel" :
            (tagId == 0xA002) ? "PixelXDimension" :
            (tagId == 0xA003) ? "PixelYDimension" :

            (tagId == 0x927C) ? "MakerNote" :
            (tagId == 0x9286) ? "UserComment" :

            (tagId == 0xA004) ? "RelatedSoundFile" :

            (tagId == 0x9003) ? "DateTimeOriginal" :
            (tagId == 0x9004) ? "DateTimeDigitized" :
            (tagId == 0x9290) ? "SubSecTime" :
            (tagId == 0x9291) ? "SubSecTimeOriginal" :
            (tagId == 0x9292) ? "SubSecTimeDigitized" :

            (tagId == 0x829A) ? "ExposureTime" :
            (tagId == 0x829D) ? "FNumber" :
            (tagId == 0x8822) ? "ExposureProgram" :
            (tagId == 0x8824) ? "SpectralSensitivity" :
            (tagId == 0x8827) ? "PhotographicSensitivity" :
            (tagId == 0x8828) ? "OECF" :
            (tagId == 0x8830) ? "SensitivityType" :
            (tagId == 0x8831) ? "StandardOutputSensitivity" :
            (tagId == 0x8832) ? "RecommendedExposureIndex" :
            (tagId == 0x8833) ? "ISOSpeed" :
            (tagId == 0x8834) ? "ISOSpeedLatitudeyyy" :
            (tagId == 0x8835) ? "ISOSpeedLatitudezzz" :

            (tagId == 0x9201) ? "ShutterSpeedValue" :
            (tagId == 0x9202) ? "ApertureValue" :
            (tagId == 0x9203) ? "BrightnessValue" :
            (tagId == 0x9204) ? "ExposureBiasValue" :
            (tagId == 0x9205) ? "MaxApertureValue" :
            (tagId == 0x9206) ? "SubjectDistance" :
            (tagId == 0x9207) ? "MeteringMode" :
            (tagId == 0x9208) ? "LightSource" :
            (tagId == 0x9209) ? "Flash" :
            (tagId == 0x920A) ? "FocalLength" :
            (tagId == 0x9214) ? "SubjectArea" :
            (tagId == 0xA20B) ? "FlashEnergy" :
            (tagId == 0xA20C) ? "SpatialFrequencyResponse" :
            (tagId == 0xA20E) ? "FocalPlaneXResolution" :
            (tagId == 0xA20F) ? "FocalPlaneYResolution" :
            (tagId == 0xA210) ? "FocalPlaneResolutionUnit" :
            (tagId == 0xA214) ? "SubjectLocation" :
            (tagId == 0xA215) ? "ExposureIndex" :
            (tagId == 0xA217) ? "SensingMethod" :
            (tagId == 0xA300) ? "FileSource" :
            (tagId == 0xA301) ? "SceneType" :
            (tagId == 0xA302) ? "CFAPattern" :

            (tagId == 0xA401) ? "CustomRendered" :
            (tagId == 0xA402) ? "ExposureMode" :
            (tagId == 0xA403) ? "WhiteBalance" :
            (tagId == 0xA404) ? "DigitalZoomRatio" :
            (tagId == 0xA405) ? "FocalLengthIn35mmFormat" :
            (tagId == 0xA406) ? "SceneCaptureType" :
            (tagId == 0xA407) ? "GainControl" :
            (tagId == 0xA408) ? "Contrast" :
            (tagId == 0xA409) ? "Saturation" :
            (tagId == 0xA40A) ? "Sharpness" :
            (tagId == 0xA40B) ? "DeviceSettingDescription" :
            (tagId == 0xA40C) ? "SubjectDistanceRange" :

            (tagId == 0xA420) ? "ImageUniqueID" :
            (tagId == 0xA430) ? "CameraOwnerName" :
            (tagId == 0xA431) ? "BodySerialNumber" :
            (tagId == 0xA432) ? "LensSpecification" :
            (tagId == 0xA433) ? "LensMake" :
            (tagId == 0xA434) ? "LensModel" :
            (tagId == 0xA435) ? "LensSerialNumber" :
            (tagId == 0xA500) ? "Gamma" : 
            "(unknown)");
    } else if (ifdType == IFD_GPS) {
        strcpy(tagName,
            (tagId == 0x0000) ? "GPSVersionID" :
            (tagId == 0x0001) ? "GPSLatitudeRef" :
            (tagId == 0x0002) ? "GPSLatitude" :
            (tagId == 0x0003) ? "GPSLongitudeRef" :
            (tagId == 0x0004) ? "GPSLongitude" :
            (tagId == 0x0005) ? "GPSAltitudeRef" :
            (tagId == 0x0006) ? "GPSAltitude" :
            (tagId == 0x0007) ? "GPSTimeStamp" :
            (tagId == 0x0008) ? "GPSSatellites" :
            (tagId == 0x0009) ? "GPSStatus" :
            (tagId == 0x000A) ? "GPSMeasureMode" :
            (tagId == 0x000B) ? "GPSDOP" :
            (tagId == 0x000C) ? "GPSSpeedRef" :
            (tagId == 0x000D) ? "GPSSpeed" :
            (tagId == 0x000E) ? "GPSTrackRef" :
            (tagId == 0x000F) ? "GPSTrack" :
            (tagId == 0x0010) ? "GPSImgDirectionRef" :
            (tagId == 0x0011) ? "GPSImgDirection" :
            (tagId == 0x0012) ? "GPSMapDatum" :
            (tagId == 0x0013) ? "GPSDestLatitudeRef" :
            (tagId == 0x0014) ? "GPSDestLatitude" :
            (tagId == 0x0015) ? "GPSDestLongitudeRef" :
            (tagId == 0x0016) ? "GPSDestLongitude" :
            (tagId == 0x0017) ? "GPSBearingRef" :
            (tagId == 0x0018) ? "GPSBearing" :
            (tagId == 0x0019) ? "GPSDestDistanceRef" :
            (tagId == 0x001A) ? "GPSDestDistance" :
            (tagId == 0x001B) ? "GPSProcessingMethod" :
            (tagId == 0x001C) ? "GPSAreaInformation" :
            (tagId == 0x001D) ? "GPSDateStamp" :
            (tagId == 0x001E) ? "GPSDifferential" :
            (tagId == 0x001F) ? "GPSHPositioningError" :
            "(unknown)");
    } else if (ifdType == IFD_IO) {
        strcpy(tagName, 
            (tagId == 0x0001) ? "InteroperabilityIndex" :
            (tagId == 0x0002) ? "InteroperabilityVersion" :
            "(unknown)");
    }
    return tagName;
}

// create the IFD table
static void *createIfdTable(IFD_TYPE IfdType, unsigned short tagCount, unsigned int nextOfs)
{
    IfdTable *ifd = (IfdTable*)malloc(sizeof(IfdTable));
    if (!ifd) {
        return NULL;
    }
    memset(ifd, 0, sizeof(IfdTable));
    ifd->ifdType = IfdType;
    ifd->tagCount = tagCount;
    ifd->nextIfdOffset = nextOfs;
    return ifd;
}

// add the TagNode enrtry to the IFD table
static void *addTagNodeToIfd(void *pIfd,
                      unsigned short tagId,
                      unsigned short type,
                      unsigned int count,
                      unsigned int *numData,
                      unsigned char *byteData)
{
    int i;
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return NULL;
    }
    tag = (TagNode*)malloc(sizeof(TagNode));
    memset(tag, 0, sizeof(TagNode));
    tag->tagId = tagId;
    tag->type = type;
    tag->count = count;

    if (count > 0) {
        if (numData != NULL) {
            int num = count;
            if (type == TYPE_RATIONAL ||
                type == TYPE_SRATIONAL) {
                num *= 2;
            }
            tag->numData = (unsigned int*)malloc(sizeof(int)*num);
            for (i = 0; i < num; i++) {
                tag->numData[i] = numData[i];
            }
        } else if (byteData != NULL) {
            tag->byteData = (unsigned char*)malloc(count);
            memcpy(tag->byteData, byteData, count);
        } else {
            tag->error = 1;
        }
    } else {
        tag->error = 1;
    }
    
    // first tag
    if (!ifd->tags) {
        ifd->tags = tag;
    } else {
        TagNode *tagWk = ifd->tags;
        while (tagWk->next) {
            tagWk = tagWk->next;
        }
        tagWk->next = tag;
        tag->prev = tagWk;
    }

    return tag;
}

// create a copy of TagNode
static TagNode *duplicateTagNode(TagNode *src)
{
    TagNode *dup;
    size_t len;
    if (!src || src->count <= 0) {
        return NULL;
    }
    dup = (TagNode*)malloc(sizeof(TagNode));
    memset(dup, 0, sizeof(TagNode));
    dup->tagId = src->tagId;
    dup->type = src->type;
    dup->count = src->count;
    dup->error = src->error;
    if (src->numData) {
        len = sizeof(int) * src->count;
        if (src->type == TYPE_RATIONAL ||
            src->type == TYPE_SRATIONAL) {
            len *= 2;
        }
        dup->numData = (unsigned int*)malloc(len);
        memcpy(dup->numData, src->numData, len);
    } else if (src->byteData) {
        len = sizeof(char) * src->count;
        dup->byteData = (unsigned char*)malloc(len);
        memcpy(dup->byteData, src->byteData, len);
    }
    return dup;
}

// free TagNode
static void freeTagNode(void *pTag)
{
    TagNode *tag = (TagNode*)pTag;
    if (!tag) {
        return;
    }
    if (tag->numData) {
        free(tag->numData);
    }
    if (tag->byteData) {
        free(tag->byteData);
    }
    free(tag);
}

// free entire IFD table
static void freeIfdTable(void *pIfd)
{
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return;
    }
    tag = ifd->tags;
    if (ifd->p) {
        free(ifd->p);
    }
    free(ifd);

    if (tag) {
        while (tag->next) {
            tag = tag->next;
        }
        while (tag) {
            TagNode *tagWk = tag->prev;
            freeTagNode(tag);
            tag = tagWk;
        }
    }
    return;
}

// search the specified tag's node from the IFD table
static TagNode *getTagNodePtrFromIfd(IfdTable *ifd, unsigned short tagId)
{
    TagNode *tag;
    if (!ifd) {
        return NULL;
    }
    tag = ifd->tags;
    while (tag) {
        if (tag->tagId == tagId) {
            return tag;
        }
        tag = tag->next;
    }
    return NULL;
}

// remove the TagNode entry from the IFD table
static int removeTagOnIfd(void *pIfd, unsigned short tagId)
{
    int num = 0;
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return 0;
    }
    for (;;) { // possibility of multiple entries
        tag = getTagNodePtrFromIfd(ifd, tagId);
        if (!tag) {
            break; // no more found
        }
        num++;
        if (tag->prev) {
            tag->prev->next = tag->next;
        } else {
            ifd->tags = tag->next;
        }
        if (tag->next) {
            tag->next->prev = tag->prev;
        }
        freeTagNode(tag);
        ifd->tagCount--;
    }
    return num;
}

// get the IFD table address of the specified type
static IfdTable *getIfdTableFromIfdTableArray(void **ifdTableArray, IFD_TYPE ifdType)
{
    int i;
    if (!ifdTableArray) {
        return NULL;
    }
    for (i = 0; ifdTableArray[i] != NULL; i++) {
		IfdTable *ifd = (IfdTable *)ifdTableArray[i];
        if (ifd->ifdType == ifdType) {
            return ifd;
        }
    }
    return NULL;
}

// count IFD tables
static int countIfdTableOnIfdTableArray(void **ifdTableArray)
{
    int i, num = 0;
    for (i = 0; ifdTableArray[i] != NULL; i++) {
        num++;
    }
    return num;
}

// set single numeric value to the existing TagNode entry
static int setSingleNumDataToTag(TagNode *tag, unsigned int value)
{
    if (!tag) {
        return 0;
    }
    if (tag->type != TYPE_BYTE   &&
        tag->type != TYPE_SHORT  &&
        tag->type != TYPE_LONG   &&
        tag->type != TYPE_SBYTE  &&
        tag->type != TYPE_SSHORT &&
        tag->type != TYPE_SLONG) {
        return 0;
    }
    if (!tag->numData) {
        tag->numData = (unsigned int*)malloc(sizeof(int));
    }
    tag->count = 1;
    tag->numData[0] = value;
    tag->error = 0;
    return 1;
}

/**
 * write the Exif segment to the file
 *
 * parameters
 *  [in] fp: the output file pointer
 *  [in] ifdTableArray: address of the IFD tables array
 *
 * return
 *  0: OK
 *  ERR_WRITE_FILE
 */
static int writeExifSegment(FILE *fp, void **ifdTableArray)
{
#define IFDMAX 5

    union _packed {
        unsigned int ui;
        unsigned short us[2];
        unsigned char uc[4];
    };

    IfdTable *ifds[IFDMAX], *ifd0th;
    TagNode *tag;
    IFD_TAG tagField;
    unsigned short num, us;
    unsigned int ui;
    int zero = 0;
    int i, x;
    unsigned int ofs;
    union _packed packed;
    APP1_HEADER dupApp1Header = App1Header;

    ifds[0] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_0TH);
    ifds[1] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_EXIF);
    ifds[2] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_IO);
    ifds[3] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_GPS);
    ifds[4] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
    ifd0th = ifds[0];

    // return if 0th IFD is not exist
    if (!ifd0th) {
        return 0;
    }
    // get total length of the segment
    us = sizeof(APP1_HEADER) - sizeof(short);
    for (x = 0; x < IFDMAX; x++) {
        if (ifds[x]) {
            us = us + ifds[x]->length;
        }
    }
    // segment length must be written in big-endian
    if (systemIsLittleEndian()) {
        us = swab16(us);
    }
    dupApp1Header.length = us;
    dupApp1Header.tiff.reserved = fix_short(dupApp1Header.tiff.reserved);
    dupApp1Header.tiff.Ifd0thOffset = fix_int(dupApp1Header.tiff.Ifd0thOffset);
    // write Exif segment Header
    if (fwrite(&dupApp1Header, 1, sizeof(APP1_HEADER), fp) != sizeof(APP1_HEADER)) {
        return ERR_WRITE_FILE;
    }

    // base offset of the Exif segment
    ofs = sizeof(TIFF_HEADER);
    for (x = 0; x < IFDMAX; x++) {
        IfdTable *ifd = ifds[x];
        if (ifd == NULL) {
            continue;
        }
        // calculate the start offset to write the tag's data of this IFD
        ofs += sizeof(short) + // sizeof the tag number area
               sizeof(IFD_TAG) * ifd->tagCount + // sizeof the tag fields
               sizeof(int);    // sizeof the NextOffset area

        // write actual tag number of the current IFD
        num = 0;
        tag = ifd->tags;
        while (tag) {
            if (!tag->error) {
                num++;
            }
            tag = tag->next;
        }
        us = fix_short(num);
        if (fwrite(&us, 1, sizeof(short), fp) != sizeof(short)) {
            return ERR_WRITE_FILE;
        }

        // write the each tag fields
        tag = ifd->tags;
        while (tag) {
            if (tag->error) {
                tag = tag->next; // ignore
                continue;
            }
            tagField.tag = fix_short(tag->tagId);
            tagField.type = fix_short(tag->type);
            tagField.count = fix_int(tag->count);
            packed.ui = 0;

            switch (tag->type) {
            case TYPE_ASCII:
            case TYPE_UNDEFINED:
                if (tag->count <= 4) {
                    for (i = 0; i < (int)tag->count; i++) {
                        packed.uc[i] = tag->byteData[i];
                    }
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count;
                    if (tag->count % 2 != 0) {
                        ofs++;
                    }
                }
                break;
            case TYPE_BYTE:
            case TYPE_SBYTE:
                if (tag->count <= 4) {
                    for (i = 0; i < (int)tag->count; i++) {
                        packed.uc[i] = (unsigned char)tag->numData[i];
                    }
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count;
                    if (tag->count % 2 != 0) {
                        ofs++;
                    }
                }
                break;
            case TYPE_SHORT:
            case TYPE_SSHORT:
                if (tag->count <= 2) {
                    for (i = 0; i < (int)tag->count; i++) {
                        packed.us[i] = fix_short((unsigned short)tag->numData[i]);
                    }
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count * sizeof(short);
                }
                break;
            case TYPE_LONG:
            case TYPE_SLONG:
                if (tag->count <= 1) {
                    packed.ui = fix_int((unsigned int)tag->numData[0]);
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count * sizeof(short);
                }
                break;
            case TYPE_RATIONAL:
            case TYPE_SRATIONAL:
                packed.ui = fix_int(ofs);
                ofs += tag->count * sizeof(int) * 2;
                break;
            }
            tagField.offset = packed.ui;
            if (fwrite(&tagField, 1, sizeof(tagField), fp) != sizeof(tagField)) {
                return ERR_WRITE_FILE;
            }
            tag = tag->next;
        }
        ui = fix_int(ifd->nextIfdOffset);
        if (fwrite(&ui, 1, sizeof(int), fp) != sizeof(int)) {
            return ERR_WRITE_FILE;
        }

        // write the tag values over 4 bytes 
        tag = ifd->tags;
        while (tag) {
            if (tag->error) {
                tag = tag->next;
                continue;
            }
            switch (tag->type) {
            case TYPE_ASCII:
            case TYPE_UNDEFINED:
                if (tag->count > 4) {
                    if (fwrite(tag->byteData, 1, tag->count, fp) != tag->count) {
                        return ERR_WRITE_FILE;
                    }
                    if (tag->count % 2 != 0) { // for even boundary
                        if (fwrite(&zero, 1, sizeof(char), fp) != sizeof(char)) {
                            return ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_BYTE:
            case TYPE_SBYTE:
                if (tag->count > 4) {
                    for (i = 0; i < (int)tag->count; i++) {
                        unsigned char n = (unsigned char)tag->numData[i];
                        if (fwrite(&n, 1, sizeof(char), fp) != sizeof(char)) {
                            return ERR_WRITE_FILE;
                        }

                    }
                    if (tag->count % 2 != 0) {
                        if (fwrite(&zero, 1, sizeof(char), fp) != sizeof(char)) {
                            return ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_SHORT:
            case TYPE_SSHORT:
                if (tag->count > 2) {
                    for (i = 0; i < (int)tag->count; i++) {
                        unsigned short n = fix_short((unsigned short)tag->numData[i]);
                        if (fwrite(&n, 1, sizeof(short), fp) != sizeof(short)) {
                            return ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_LONG:
            case TYPE_SLONG:
                if (tag->count > 1) {
                    for (i = 0; i < (int)tag->count; i++) {
                        unsigned int n = fix_int((unsigned int)tag->numData[i]);
                        if (fwrite(&n, 1, sizeof(int), fp) != sizeof(int)) {
                            return ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_RATIONAL:
            case TYPE_SRATIONAL:
                for (i = 0; i < (int)tag->count*2; i++) {
                    unsigned int n = fix_int((unsigned int)tag->numData[i]);
                    if (fwrite(&n, 1, sizeof(int), fp) != sizeof(int)) {
                        return ERR_WRITE_FILE;
                    }
                }
                break;
            }
            tag = tag->next;
        }
        // write the thumbnail data in the 1st IFD
        if (ifd->ifdType == IFD_1ST && ifd->p != NULL) {
            tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                if (tag->numData[0] > 0) {
                    if (fwrite(ifd->p, 1, tag->numData[0], fp) != tag->numData[0]) {
                        return ERR_WRITE_FILE;
                    }
                }
            }
        }
    }
    return 0;
}

// calculate the actual length of the IFD
static unsigned short calcIfdSize(void *pIfd)
{
    unsigned int size, num = 0;
    TagNode *tag;
    IfdTable *ifd = (IfdTable*)pIfd;
    if (!ifd) {
        return 0;
    }
    // count the actual tag number
    tag = ifd->tags;
    while (tag) {
        if (!tag->error) {
            num++;
        }
        tag = tag->next;
    }

    size = sizeof(short) + // sizeof the tag number area
           sizeof(IFD_TAG) * num + // sizeof tag fields
           sizeof(int); // sizeof the NextOffset area

    // add thumbnail data length
    if (ifd->ifdType == IFD_1ST) {
        if (ifd->p) {
            tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                size += tag->numData[0];
            }
        }
    }
    tag = ifd->tags;
    while (tag) {
        if (tag->error) {
            // ignore
            tag = tag->next;
            continue;
        }
        switch (tag->type) {
        case TYPE_ASCII:
        case TYPE_UNDEFINED:
        case TYPE_BYTE:
        case TYPE_SBYTE:
            if (tag->count > 4) {
                size += tag->count;
                if (tag->count % 2 != 0) {
                    // padding for even byte boundary
                    size += 1;
                }
            }
            break;
        case TYPE_SHORT:
        case TYPE_SSHORT:
            if (tag->count > 2) {
                size += tag->count * sizeof(short);
            }
            break;
        case TYPE_LONG:
        case TYPE_SLONG:
            if (tag->count > 1) {
                size += tag->count * sizeof(int);
            }
            break;
        case TYPE_RATIONAL:
        case TYPE_SRATIONAL:
            if (tag->count > 0) {
                size += tag->count * sizeof(int) * 2;
            }
            break;
        }
        tag = tag->next;
    }
    return (unsigned short)size;
}

/**
 * refresh the length and offset variables in the IFD tables
 *
 * parameters
 *  [in/out] ifdTableArray: address of the IFD tables array
 *
 * return
 *  0: OK
 *  ERR_INVALID_POINTER
 *  ERROR_UNKNOWN
 */
static int fixLengthAndOffsetInIfdTables(void **ifdTableArray)
{
    int i;
    TagNode *tag, *tagwk;
    unsigned short num;
    unsigned short ofsBase = sizeof(TIFF_HEADER);
    unsigned int len, dummy = 0, again = 0;
    IfdTable *ifd0th, *ifdExif, *ifdIo, *ifdGps, *ifd1st; 
    if (!ifdTableArray) {
        return ERR_INVALID_POINTER;
    }

AGAIN:
    // calculate the length of the each IFD tables.
    for (i = 0; ifdTableArray[i] != NULL; i++) {
		IfdTable *ifd = (IfdTable *)ifdTableArray[i];
        // count the actual tag number
        tag = ifd->tags;
        num = 0;
        while (tag) {
            // ignore and dispose the error tag
            if (tag->error) {
                tagwk = tag->next;
                if (tag->prev) {
                    tag->prev->next = tag->next;
                } else {
                    ifd->tags = tag->next;
                }
                if (tag->next) {
                    tag->next->prev = tag->prev;
                }
                freeTagNode(tag);
                tag = tagwk;
                continue;
            }
            num++;
            tag = tag->next;
        }
        ifd->tagCount = num;
        ifd->length = calcIfdSize(ifd);
        ifd->nextIfdOffset = 0;
    }
    ifd0th  = getIfdTableFromIfdTableArray(ifdTableArray, IFD_0TH);
    ifdExif = getIfdTableFromIfdTableArray(ifdTableArray, IFD_EXIF);
    ifdIo   = getIfdTableFromIfdTableArray(ifdTableArray, IFD_IO);
    ifdGps  = getIfdTableFromIfdTableArray(ifdTableArray, IFD_GPS);
    ifd1st  = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);

    if (!ifd0th) {
        return 0; // not error
    }
    ifd0th->offset = ofsBase;

    // set "NextOffset" variable in the 0th IFD if the 1st IFD is exist
    if (ifd1st) {
        ifd0th->nextIfdOffset =
                ofsBase +
                ifd0th->length + 
                ((ifdExif)? ifdExif->length : 0) +
                ((ifdIo)? ifdIo->length : 0) +
                ((ifdGps)? ifdGps->length : 0);
        ifd1st->offset = (unsigned short)ifd0th->nextIfdOffset;

        // set the offset value of the thumbnail data
        if (ifd1st->p) {
            tag = getTagNodePtrFromIfd(ifd1st, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                len = tag->numData[0]; // thumbnail length;
                tag = getTagNodePtrFromIfd(ifd1st, TAG_JPEGInterchangeFormat);
                if (tag) {
                    // set the offset value
                    setSingleNumDataToTag(tag, ifd1st->offset + ifd1st->length - len);
                } else {
                    // create the JPEGInterchangeFormat tag if not exist
                    if (!addTagNodeToIfd(ifd1st, TAG_JPEGInterchangeFormat, 
                        TYPE_LONG, 1, &dummy, NULL)) {
                        return ERR_UNKNOWN;
                    }
                    again = 1;
                }
            } else {
                tag = getTagNodePtrFromIfd(ifd1st, TAG_JPEGInterchangeFormat);
                if (tag) {
                    setSingleNumDataToTag(tag, 0);
                }
            }
        }
    } else {
        ifd0th->nextIfdOffset = 0; // 1st IFD is not exist
    }

    // set "ExifIFDPointer" tag value in the 0th IFD if the Exif IFD is exist
    if (ifdExif) {
        tag = getTagNodePtrFromIfd(ifd0th, TAG_ExifIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, ofsBase + ifd0th->length);
            ifdExif->offset = (unsigned short)tag->numData[0];
        } else {
            // create the tag if not exist
            if (!addTagNodeToIfd(ifd0th, TAG_ExifIFDPointer, TYPE_LONG, 1,
                                    &dummy, NULL)) {
                return ERR_UNKNOWN;
            }
            again = 1;
        }
        // set "InteroperabilityIFDPointer" tag value in the Exif IFD
        // if the Interoperability IFD is exist
        if (ifdIo) {
            tag = getTagNodePtrFromIfd(ifdExif, TAG_InteroperabilityIFDPointer);
            if (tag) {
                setSingleNumDataToTag(tag, ofsBase + ifd0th->length + ifdExif->length);
                ifdIo->offset = (unsigned short)tag->numData[0];
            } else {
                // create the tag if not exist
                if (!addTagNodeToIfd(ifdExif, TAG_InteroperabilityIFDPointer,
                                        TYPE_LONG, 1, &dummy, NULL)) {
                    return ERR_UNKNOWN;
                }
                again = 1;
            }
        } else {
            tag = getTagNodePtrFromIfd(ifdExif, TAG_InteroperabilityIFDPointer);
            if (tag) {
                setSingleNumDataToTag(tag, 0);
            }
        }
    } else { // Exif 
        tag = getTagNodePtrFromIfd(ifd0th, TAG_ExifIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, 0);
        }
    }

    // set "GPSInfoIFDPointer" tag value in the 0th IFD if the GPS IFD is exist
    if (ifdGps) {
        tag = getTagNodePtrFromIfd(ifd0th, TAG_GPSInfoIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, ofsBase +
                                        ifd0th->length + 
                                        ((ifdExif)? ifdExif->length : 0) +
                                        ((ifdIo)? ifdIo->length : 0));
            ifdGps->offset = (unsigned short)tag->numData[0];
        } else {
            // create the tag if not exist
            if (!addTagNodeToIfd(ifd0th, TAG_GPSInfoIFDPointer, TYPE_LONG,
                                1, &dummy, NULL)) {
                return ERR_UNKNOWN;
            }
            again = 1;
        }
    } else { // GPS IFD is not exist
        tag = getTagNodePtrFromIfd(ifd0th, TAG_GPSInfoIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, 0);
        }
    }
    // repeat again if needed
    if (again) {
        again = 0;
        goto AGAIN;
    }
    return 0;
}

/**
 * Set the data of the IFD to the internal table
 *
 * parameters
 *  [in] fp: file pointer of opened file
 *  [in] startOffset : offset of target IFD
 *  [in] ifdType : type of the IFD
 *
 * return
 *   NULL: critical error occurred
 *  !NULL: the address of the IFD table
 */
static void *parseIFD(FILE *fp,
                      unsigned int startOffset,
                      IFD_TYPE ifdType)
{
    void *ifd;
    unsigned char buf[8192];
    unsigned short tagCount, us;
    unsigned int nextOffset = 0;
    unsigned int *array, val, allocSize;
    int size, cnt, i;
    size_t len;
    int pos;
    
    // get the count of the tags
    if (seekToRelativeOffset(fp, startOffset) != 0 ||
        fread(&tagCount, 1, sizeof(short), fp) < sizeof(short)) {
        return NULL;
    }
    tagCount = fix_short(tagCount);
    pos = ftell(fp);

    // in case of the 0th IFD, check the offset of the 1st IFD
    if (ifdType == IFD_0TH) {
        // next IFD's offset is at the tail of the segment
        if (seekToRelativeOffset(fp,
                sizeof(TIFF_HEADER) + sizeof(short) + sizeof(IFD_TAG) * tagCount) != 0 ||
            fread(&nextOffset, 1, sizeof(int), fp) < sizeof(int)) {
            return NULL;
        }
        nextOffset = fix_int(nextOffset);
        fseek(fp, pos, SEEK_SET);
    }
    // create new IFD table
    ifd = createIfdTable(ifdType, tagCount, nextOffset);

    // parse all tags
    for (cnt = 0; cnt < tagCount; cnt++) {
        IFD_TAG tag;
        unsigned char data[4];
        if (fseek(fp, pos, SEEK_SET) != 0 ||
            fread(&tag, 1, sizeof(tag), fp) < sizeof(tag)) {
            goto ERR;
        }
        memcpy(data, &tag.offset, 4); // keep raw data temporary
        tag.tag = fix_short(tag.tag);
        tag.type = fix_short(tag.type);
        tag.count = fix_int(tag.count);
        tag.offset = fix_int(tag.offset);
        pos = ftell(fp);

        //printf("tag=0x%04X type=%u count=%u offset=%u name=[%s]\n",
        //  tag.tag, tag.type, tag.count, tag.offset, getTagName(ifdType, tag.tag));

        if (tag.type == TYPE_ASCII ||     // ascii = the null-terminated string
            tag.type == TYPE_UNDEFINED) { // undefined = the chunk data bytes
            if (tag.count <= 4)  {
                // 4 bytes or less data is placed in the 'offset' area directly
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, data);
            } else {
                // 5 bytes or more data is placed in the value area of the IFD
                unsigned char *p = buf;
                if (tag.count > sizeof(buf)) {
                    // allocate new buffer if needed
                    if (tag.count >= App1Header.length) { // illegal
                        p = NULL;
                    } else {
                        p = (unsigned char*)malloc(tag.count);
                    }
                    if (!p) {
                        // treat as an error
                        addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                        continue;
                    }
                    memset(p, 0, tag.count);
                }
                if (seekToRelativeOffset(fp, tag.offset) != 0 ||
                    fread(p, 1, tag.count, fp) < tag.count) {
                    if (p != &buf[0]) {
                        free(p);
                    }
                    addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                    continue;
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, p);
                if (p != &buf[0]) {
                    free(p);
                }
            }
        }
        else if (tag.type == TYPE_RATIONAL || tag.type == TYPE_SRATIONAL) {
            unsigned int realCount = tag.count * 2; // need double the space
            size_t len = realCount * sizeof(int);
            if (len >= App1Header.length) { // illegal
                array = NULL;
            } else {
                array = (unsigned int*)malloc(len);
                if (array) {
                    if (seekToRelativeOffset(fp, tag.offset) != 0 ||
                        fread(array, 1, len , fp) < len) {
                        free(array);
                        array = NULL;
                    } else {
                        for (i = 0; i < (int)realCount; i++) {
                            array[i] = fix_int(array[i]);
                        }
                    }
                }
            }
            addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, array, NULL);
            if (array) {
                free(array);
            }
        }
        else if (tag.type == TYPE_BYTE   ||
                 tag.type == TYPE_SHORT  ||
                 tag.type == TYPE_LONG   ||
                 tag.type == TYPE_SBYTE  ||
                 tag.type == TYPE_SSHORT ||
                 tag.type == TYPE_SLONG ) {

            // the single value is always stored in tag.offset area directly
            // # the data is Left-justified if less than 4 bytes
            if (tag.count <= 1) {
                val = tag.offset;
                if (tag.type == TYPE_BYTE || tag.type == TYPE_SBYTE) {
                    unsigned char uc = data[0];
                    val = uc;
                } else if (tag.type == TYPE_SHORT || tag.type == TYPE_SSHORT) {
                    memcpy(&us, data, sizeof(short));
                    us = fix_short(us);
                    val = us;
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, &val, NULL);
             }
             // multiple value
             else {
                size = sizeof(int);
                if (tag.type == TYPE_BYTE || tag.type == TYPE_SBYTE) {
                    size = sizeof(char);
                } else if (tag.type == TYPE_SHORT || tag.type == TYPE_SSHORT) {
                    size = sizeof(short);
                }
                // for the sake of simplicity, using the 4bytes area for
                // each numeric data type 
                allocSize = sizeof(int) * tag.count;
                if (allocSize >= App1Header.length) { // illegal
                    array = NULL;
                } else {
                    array = (unsigned int*)malloc(allocSize);
                }
                if (!array) {
                    addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                    continue;
                }
                len = size * tag.count;
                // if the total length of the value is less than or equal to 4bytes, 
                // they have been stored in the tag.offset area
                if (len <= 4) {
                    if (size == 1) { // byte
                        for (i = 0; i < (int)tag.count; i++) {
                            array[i] = (unsigned int)data[i];
                        }
                    } else if (size == 2) { // short
                        for (i = 0; i < 2; i++) {
                            memcpy(&us, &data[i*2], sizeof(short));
                            us = fix_short(us);
                            array[i] = (unsigned int)us;
                        }
                    }
                } else {
                    if (seekToRelativeOffset(fp, tag.offset) != 0 ||
                        fread(buf, 1, len , fp) < len) {
                        addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                        continue;
                    }
                    for (i = 0; i < (int)tag.count; i++) {
                        memcpy(&val, &buf[i*size], size);
                        if (size == sizeof(int)) {
                            val = fix_int(val);
                        } else if (size == sizeof(short)) {
                            val = fix_short((unsigned short)val);
                        }
                        array[i] = (unsigned int)val;
                    }
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, array, NULL);
                free(array);
             }
         }
    }
    if (ifdType == IFD_1ST) {
        // get thumbnail data
        unsigned int thumbnail_ofs = 0, thumbnail_len;
        IfdTable *ifdTable = (IfdTable*)ifd;
		TagNode *tag = getTagNodePtrFromIfd((IfdTable *)ifd, TAG_JPEGInterchangeFormat);
        if (tag) {
            thumbnail_ofs = tag->numData[0];
        }
        if (thumbnail_ofs > 0) {
			tag = getTagNodePtrFromIfd((IfdTable *)ifd, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                thumbnail_len = tag->numData[0];
                if (thumbnail_len > 0) {
                    ifdTable->p = (unsigned char*)malloc(thumbnail_len);
                    if (ifdTable->p) {
                        if (seekToRelativeOffset(fp, thumbnail_ofs) == 0) {
                            if (fread(ifdTable->p, 1, thumbnail_len, fp)
                                                        != thumbnail_len) {
                                free(ifdTable->p);
                                ifdTable->p = NULL;
                            } else {
                                // for test
                                //FILE *fpw = fopen("thumbnail.jpg", "wb");
                                //fwrite(ifdTable->p, 1, thumbnail_len, fpw);
                                //fclose(fpw);
                            }
                        } else {
                            free(ifdTable->p);
                            ifdTable->p = NULL;
                        }
                    }
                }
            }
        }
    }
    return ifd;
ERR:
    if (ifd) {
        freeIfdTable(ifd);
    }
    return NULL;
}


void setDefaultApp1SegmentHader()
{
    memset(&App1Header, 0, sizeof(APP1_HEADER));
    App1Header.marker = (systemIsLittleEndian()) ? 0xE1FF : 0xFFE1;
    App1Header.length = 0;
    strcpy(App1Header.id, "Exif");
    App1Header.tiff.byteOrder = 0x4949; // means little-endian
    App1Header.tiff.reserved = 0x002A;
    App1Header.tiff.Ifd0thOffset = 0x00000008;
}

/**
 * Load the APP1 segment header
 *
 * return
 *  1: success
 *  0: error
 */
static int readApp1SegmentHeader(FILE *fp)
{
    // read the APP1 header
    if (fseek(fp, App1StartOffset, SEEK_SET) != 0 ||
        fread(&App1Header, 1, sizeof(APP1_HEADER), fp) <
                                            sizeof(APP1_HEADER)) {
        return 0;
    }
    if (systemIsLittleEndian()) {
        // the segment length value is always in big-endian order
        App1Header.length = swab16(App1Header.length);
    }
    // byte-order identifier
    if (App1Header.tiff.byteOrder != 0x4D4D && // big-endian
        App1Header.tiff.byteOrder != 0x4949) { // little-endian
        return 0;
    }
    // TIFF version number (always 0x002A)
    App1Header.tiff.reserved = fix_short(App1Header.tiff.reserved);
    if (App1Header.tiff.reserved != 0x002A) {
        return 0;
    }
    // offset of the 0TH IFD
    App1Header.tiff.Ifd0thOffset = fix_int(App1Header.tiff.Ifd0thOffset);
    return 1;
}

/**
 * Get the offset of the Exif segment in the current opened JPEG file
 *
 * return
 *   n: the offset from the beginning of the file
 *   0: the Exif segment is not found
 *  -n: error
 */
static int getApp1StartOffset(FILE *fp,
                              const char *App1IDString,
                              size_t App1IDStringLength,
                              int *pDQTOffset)
{
    #define EXIF_ID_STR     "Exif\0"
    #define EXIF_ID_STR_LEN 5

    int pos;
    unsigned char buf[64];
    unsigned short len, marker;
    if (!fp) {
        return ERR_READ_FILE;
    }
    rewind(fp);

    // check JPEG SOI Marker (0xFFD8)
    if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
        return ERR_READ_FILE;
    }
    if (systemIsLittleEndian()) {
        marker = swab16(marker);
    }
    if (marker != 0xFFD8) {
        return ERR_INVALID_JPEG;
    }
    // check for next 2 bytes
    if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
        return ERR_READ_FILE;
    }
    if (systemIsLittleEndian()) {
        marker = swab16(marker);
    }
    // if DQT marker (0xFFDB) is appeared, the application segment
    // doesn't exist
    if (marker == 0xFFDB) {
        if (pDQTOffset != NULL) {
            *pDQTOffset = ftell(fp) - sizeof(short);
        }
        return 0; // not found the Exif segment
    }

    pos = ftell(fp);
    for (;;) {
        // unexpected value. is not a APP[0-14] marker
        if (!(marker >= 0xFFE0 && marker <= 0xFFEF)) {
            // found DQT
            if (marker == 0xFFDB && pDQTOffset != NULL) {
                *pDQTOffset = pos - sizeof(short);
            }
            break;
        }
        // read the length of the segment
        if (fread(&len, 1, sizeof(short), fp) < sizeof(short)) {
            return ERR_READ_FILE;
        }
        if (systemIsLittleEndian()) {
            len = swab16(len);
        }
        // if is not a APP1 segment, move to next segment
        if (marker != 0xFFE1) {
            if (fseek(fp, len - sizeof(short), SEEK_CUR) != 0) {
                return ERR_INVALID_JPEG;
            }
        } else {
            // check if it is the Exif segment
            if (fread(&buf, 1, App1IDStringLength, fp) < App1IDStringLength) {
                return ERR_READ_FILE;
            }
            if (memcmp(buf, App1IDString, App1IDStringLength) == 0) {
                // return the start offset of the Exif segment
                return pos - sizeof(short);
            }
            // if is not a Exif segment, move to next segment
            if (fseek(fp, pos, SEEK_SET) != 0 ||
                fseek(fp, len, SEEK_CUR) != 0) {
                return ERR_INVALID_JPEG;
            }
        }
        // read next marker
        if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
            return ERR_READ_FILE;
        }
        if (systemIsLittleEndian()) {
            marker = swab16(marker);
        }
        pos = ftell(fp);
    }
    return 0; // not found the Exif segment
}

/**
 * Initialize
 *
 * return
 *   1: OK
 *   0: the Exif segment is not found
 *  -n: error
 */
static int init(FILE *fp)
{
    int sts, dqtOffset = -1;;
    setDefaultApp1SegmentHader();
    // get the offset of the Exif segment
    sts = getApp1StartOffset(fp, EXIF_ID_STR, EXIF_ID_STR_LEN, &dqtOffset);
    if (sts < 0) { // error
        return sts;
    }
    JpegDQTOffset = dqtOffset;
    App1StartOffset = sts;
    if (sts == 0) {
        return sts;
    }
    // Load the segment header
    if (!readApp1SegmentHeader(fp)) {
        return ERR_INVALID_APP1HEADER;
    }
    return 1;
}

static void PRINTF(char **ms, const char *fmt, ...) {
    char buf[4096];
    char *p = NULL;
    int len, cnt;
    va_list args;
    va_start(args, fmt);
    cnt = vsnprintf(buf, sizeof(buf)-1, fmt, args);
    if (!ms) {
        printf("%s", buf);
        return;
    }
    else if (*ms) {
        len = (int)(strlen(*ms) + cnt + 1);
        p = (char*)malloc(len);
        strcpy(p, *ms);
        strcat(p, buf);
        free(*ms);
    } else {
        len = cnt + 1;
        p = (char*)malloc(len);
        strcpy(p, buf);
    }
    *ms = p;
    va_end(args);
}
//get the shooting angle of picture
int getImgOrientation(const char* path)
{
	void **ifdArray;
	int i, result;

	ifdArray = createIfdTableArray(path, &result);

	switch (result) {
	case 0: // no IFDs
		printf("[%s] does not seem to contain the Exif segment.\n", path);
		break;
	case ERR_READ_FILE:
		printf("failed to open or read [%s].\n", path);
		break;
	case ERR_INVALID_JPEG:
		printf("[%s] is not a valid JPEG file.\n", path);
		break;
	case ERR_INVALID_APP1HEADER:
		printf("[%s] does not have valid Exif segment header.\n", path);
		break;
	case ERR_INVALID_IFD:
		printf("[%s] contains one or more IFD errors. use -v for details.\n", path);
		break;
	default:
		//printf("[%s] createIfdTableArray: result=%d\n", path, result);
		break;
	}

	if (!ifdArray) {
		return 0;
	}

	int Ori = dumpIfdTable(ifdArray[0]);

	// free IFD table array
	if (ifdArray != NULL)
	{
		freeIfdTableArray(ifdArray);
	}

	return Ori;
}
//get picture shooting date ,then parse by ordered type.
std::string getImgData(const char* path)
{
	// get [DateTimeOriginal] tag value from Exif IFD
	void **ifdArray;
	TagNodeInfo *tag;
	int result;
	std::string str_result="";
	// parse the JPEG header and create the pointer array of the IFD tables
	ifdArray = createIfdTableArray(path, &result);
	tag = getTagInfo(ifdArray, IFD_EXIF, TAG_DateTimeOriginal);
	if (tag) {
		if (!tag->error) {
			//printf("Exif IFD : DateTimeOriginal = [%s]\n", tag->byteData);
			char *date = (char*)(tag->byteData);
			str_result = std::string(date);
		}
		freeTagInfo(tag);
	}
	if (ifdArray != NULL)
	{
		freeIfdTableArray(ifdArray);
	}

	return str_result;
}
