#include "pulp/Mp3.h"
#include "pulp/Stretch.h"
#include <new>

// MPEG audio Layer III, written from the format's definition (ISO/IEC 11172-3 and 13818-3, and the
// usual MPEG-2.5 extension to the three lowest sample rates). The tables below are the standard's own.

namespace pulp {

namespace {

// ------------------------------------------------------------------ tables shared by decoder and encoder

// kbps by bitrate index, for MPEG-1 and for MPEG-2 / 2.5. Index 0 is "free format": any rate, stated nowhere.
const int kBitrates[2][15] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
                              {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
// Sample rates in the order used for every table below: MPEG-1, MPEG-2, MPEG-2.5, each by the header's rate index.
const int kRates[9] = {44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000};

// Where the scalefactor bands start, for long blocks (lines of 576) and short blocks (lines of 192).
const short kSfbLong[9][23] = {
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576},
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576},
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576},
    {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
    {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576},
    {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
    {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
    {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
    {0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576}};
const short kSfbShort[9][14] = {
    {0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192},
    {0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192},
    {0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192},
    {0, 4, 8, 12, 18, 24, 32, 42, 56, 74, 100, 132, 174, 192},
    {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 136, 180, 192},
    {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192},
    {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192},
    {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192},
    {0, 8, 16, 24, 36, 52, 72, 96, 124, 160, 162, 164, 166, 192}};

const unsigned char kPretab[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0};
// How many bits the two groups of scalefactors take in MPEG-1, by scalefac_compress.
const unsigned char kSlen[2][16] = {{0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4}, {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3}};
// MPEG-2: how many scalefactors each of the four groups holds, by coding table and block kind (long, short, mixed).
const unsigned char kLsfGroups[6][3][4] = {
    {{6, 5, 5, 5}, {9, 9, 9, 9}, {6, 9, 9, 9}},
    {{6, 5, 7, 3}, {9, 9, 12, 6}, {6, 9, 12, 6}},
    {{11, 10, 0, 0}, {18, 18, 0, 0}, {15, 18, 0, 0}},
    {{7, 7, 7, 0}, {12, 12, 12, 0}, {6, 15, 12, 0}},
    {{6, 6, 6, 3}, {12, 9, 9, 6}, {6, 12, 9, 6}},
    {{8, 8, 5, 0}, {15, 12, 9, 0}, {6, 18, 9, 0}}};

// The Huffman code tables for pairs of values: the code of (x, y) is at [x * size + y].
const unsigned short kH1c[] = {1, 1, 1, 0};
const unsigned char kH1l[] = {1, 3, 2, 3};
const unsigned short kH2c[] = {1, 2, 1, 3, 1, 1, 3, 2, 0};
const unsigned char kH2l[] = {1, 3, 6, 3, 3, 5, 5, 5, 6};
const unsigned short kH3c[] = {3, 2, 1, 1, 1, 1, 3, 2, 0};
const unsigned char kH3l[] = {2, 2, 6, 3, 2, 5, 5, 5, 6};
const unsigned short kH5c[] = {1, 2, 6, 5, 3, 1, 4, 4, 7, 5, 7, 1, 6, 1, 1, 0};
const unsigned char kH5l[] = {1, 3, 6, 7, 3, 3, 6, 7, 6, 6, 7, 8, 7, 6, 7, 8};
const unsigned short kH6c[] = {7, 3, 5, 1, 6, 2, 3, 2, 5, 4, 4, 1, 3, 3, 2, 0};
const unsigned char kH6l[] = {3, 3, 5, 7, 3, 2, 4, 5, 4, 4, 5, 6, 6, 5, 6, 7};
const unsigned short kH7c[] = {1, 2, 10, 19, 16, 10, 3, 3, 7, 10, 5, 3, 11, 4, 13, 17, 8, 4, 12, 11, 18, 15, 11, 2, 7, 6, 9, 14, 3, 1, 6, 4, 5, 3, 2, 0};
const unsigned char kH7l[] = {1, 3, 6, 8, 8, 9, 3, 4, 6, 7, 7, 8, 6, 5, 7, 8, 8, 9, 7, 7, 8, 9, 9, 9, 7, 7, 8, 9, 9, 10, 8, 8, 9, 10, 10, 10};
const unsigned short kH8c[] = {3, 4, 6, 18, 12, 5, 5, 1, 2, 16, 9, 3, 7, 3, 5, 14, 7, 3, 19, 17, 15, 13, 10, 4, 13, 5, 8, 11, 5, 1, 12, 4, 4, 1, 1, 0};
const unsigned char kH8l[] = {2, 3, 6, 8, 8, 9, 3, 2, 4, 8, 8, 8, 6, 4, 6, 8, 8, 9, 8, 8, 8, 9, 9, 10, 8, 7, 8, 9, 10, 10, 9, 8, 9, 9, 11, 11};
const unsigned short kH9c[] = {7, 5, 9, 14, 15, 7, 6, 4, 5, 5, 6, 7, 7, 6, 8, 8, 8, 5, 15, 6, 9, 10, 5, 1, 11, 7, 9, 6, 4, 1, 14, 4, 6, 2, 6, 0};
const unsigned char kH9l[] = {3, 3, 5, 6, 8, 9, 3, 3, 4, 5, 6, 8, 4, 4, 5, 6, 7, 8, 6, 5, 6, 7, 7, 8, 7, 6, 7, 7, 8, 9, 8, 7, 8, 8, 9, 9};
const unsigned short kH10c[] = {
    1, 2, 10, 23, 35, 30, 12, 17, 3, 3, 8, 12, 18, 21, 12, 7, 11, 9, 15, 21, 32, 40, 19, 6, 14, 13, 22, 34, 46, 23, 18, 7,
    20, 19, 33, 47, 27, 22, 9, 3, 31, 22, 41, 26, 21, 20, 5, 3, 14, 13, 10, 11, 16, 6, 5, 1, 9, 8, 7, 8, 4, 4, 2, 0};
const unsigned char kH10l[] = {
    1, 3, 6, 8, 9, 9, 9, 10, 3, 4, 6, 7, 8, 9, 8, 8, 6, 6, 7, 8, 9, 10, 9, 9, 7, 7, 8, 9, 10, 10, 9, 10,
    8, 8, 9, 10, 10, 10, 10, 10, 9, 9, 10, 10, 11, 11, 10, 11, 8, 8, 9, 10, 10, 10, 11, 11, 9, 8, 9, 10, 10, 11, 11, 11};
const unsigned short kH11c[] = {
    3, 4, 10, 24, 34, 33, 21, 15, 5, 3, 4, 10, 32, 17, 11, 10, 11, 7, 13, 18, 30, 31, 20, 5, 25, 11, 19, 59, 27, 18, 12, 5,
    35, 33, 31, 58, 30, 16, 7, 5, 28, 26, 32, 19, 17, 15, 8, 14, 14, 12, 9, 13, 14, 9, 4, 1, 11, 4, 6, 6, 6, 3, 2, 0};
const unsigned char kH11l[] = {
    2, 3, 5, 7, 8, 9, 8, 9, 3, 3, 4, 6, 8, 8, 7, 8, 5, 5, 6, 7, 8, 9, 8, 8, 7, 6, 7, 9, 8, 10, 8, 9,
    8, 8, 8, 9, 9, 10, 9, 10, 8, 8, 9, 10, 10, 11, 10, 11, 8, 7, 7, 8, 9, 10, 10, 10, 8, 7, 8, 9, 10, 10, 10, 10};
const unsigned short kH12c[] = {
    9, 6, 16, 33, 41, 39, 38, 26, 7, 5, 6, 9, 23, 16, 26, 11, 17, 7, 11, 14, 21, 30, 10, 7, 17, 10, 15, 12, 18, 28, 14, 5,
    32, 13, 22, 19, 18, 16, 9, 5, 40, 17, 31, 29, 17, 13, 4, 2, 27, 12, 11, 15, 10, 7, 4, 1, 27, 12, 8, 12, 6, 3, 1, 0};
const unsigned char kH12l[] = {
    4, 3, 5, 7, 8, 9, 9, 9, 3, 3, 4, 5, 7, 7, 8, 8, 5, 4, 5, 6, 7, 8, 7, 8, 6, 5, 6, 6, 7, 8, 8, 8,
    7, 6, 7, 7, 8, 8, 8, 9, 8, 7, 8, 8, 8, 9, 8, 9, 8, 7, 7, 8, 8, 9, 9, 10, 9, 8, 8, 9, 9, 9, 9, 10};
const unsigned short kH13c[] = {
    1, 5, 14, 21, 34, 51, 46, 71, 42, 52, 68, 52, 67, 44, 43, 19,
    3, 4, 12, 19, 31, 26, 44, 33, 31, 24, 32, 24, 31, 35, 22, 14,
    15, 13, 23, 36, 59, 49, 77, 65, 29, 40, 30, 40, 27, 33, 42, 16,
    22, 20, 37, 61, 56, 79, 73, 64, 43, 76, 56, 37, 26, 31, 25, 14,
    35, 16, 60, 57, 97, 75, 114, 91, 54, 73, 55, 41, 48, 53, 23, 24,
    58, 27, 50, 96, 76, 70, 93, 84, 77, 58, 79, 29, 74, 49, 41, 17,
    47, 45, 78, 74, 115, 94, 90, 79, 69, 83, 71, 50, 59, 38, 36, 15,
    72, 34, 56, 95, 92, 85, 91, 90, 86, 73, 77, 65, 51, 44, 43, 42,
    43, 20, 30, 44, 55, 78, 72, 87, 78, 61, 46, 54, 37, 30, 20, 16,
    53, 25, 41, 37, 44, 59, 54, 81, 66, 76, 57, 54, 37, 18, 39, 11,
    35, 33, 31, 57, 42, 82, 72, 80, 47, 58, 55, 21, 22, 26, 38, 22,
    53, 25, 23, 38, 70, 60, 51, 36, 55, 26, 34, 23, 27, 14, 9, 7,
    34, 32, 28, 39, 49, 75, 30, 52, 48, 40, 52, 28, 18, 17, 9, 5,
    45, 21, 34, 64, 56, 50, 49, 45, 31, 19, 12, 15, 10, 7, 6, 3,
    48, 23, 20, 39, 36, 35, 53, 21, 16, 23, 13, 10, 6, 1, 4, 2,
    16, 15, 17, 27, 25, 20, 29, 11, 17, 12, 16, 8, 1, 1, 0, 1};
const unsigned char kH13l[] = {
    1, 4, 6, 7, 8, 9, 9, 10, 9, 10, 11, 11, 12, 12, 13, 13,
    3, 4, 6, 7, 8, 8, 9, 9, 9, 9, 10, 10, 11, 12, 12, 12,
    6, 6, 7, 8, 9, 9, 10, 10, 9, 10, 10, 11, 11, 12, 13, 13,
    7, 7, 8, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 13,
    8, 7, 9, 9, 10, 10, 11, 11, 10, 11, 11, 12, 12, 13, 13, 14,
    9, 8, 9, 10, 10, 10, 11, 11, 11, 11, 12, 11, 13, 13, 14, 14,
    9, 9, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 13, 13, 14, 14,
    10, 9, 10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 14, 16, 16,
    9, 8, 9, 10, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14, 15, 15,
    10, 9, 10, 10, 11, 11, 11, 13, 12, 13, 13, 14, 14, 14, 16, 15,
    10, 10, 10, 11, 11, 12, 12, 13, 12, 13, 14, 13, 14, 15, 16, 17,
    11, 10, 10, 11, 12, 12, 12, 12, 13, 13, 13, 14, 15, 15, 15, 16,
    11, 11, 11, 12, 12, 13, 12, 13, 14, 14, 15, 15, 15, 16, 16, 16,
    12, 11, 12, 13, 13, 13, 14, 14, 14, 14, 14, 15, 16, 15, 16, 16,
    13, 12, 12, 13, 13, 13, 15, 14, 14, 17, 15, 15, 15, 17, 16, 16,
    12, 12, 13, 14, 14, 14, 15, 14, 15, 15, 16, 16, 19, 18, 19, 16};
const unsigned short kH15c[] = {
    7, 12, 18, 53, 47, 76, 124, 108, 89, 123, 108, 119, 107, 81, 122, 63,
    13, 5, 16, 27, 46, 36, 61, 51, 42, 70, 52, 83, 65, 41, 59, 36,
    19, 17, 15, 24, 41, 34, 59, 48, 40, 64, 50, 78, 62, 80, 56, 33,
    29, 28, 25, 43, 39, 63, 55, 93, 76, 59, 93, 72, 54, 75, 50, 29,
    52, 22, 42, 40, 67, 57, 95, 79, 72, 57, 89, 69, 49, 66, 46, 27,
    77, 37, 35, 66, 58, 52, 91, 74, 62, 48, 79, 63, 90, 62, 40, 38,
    125, 32, 60, 56, 50, 92, 78, 65, 55, 87, 71, 51, 73, 51, 70, 30,
    109, 53, 49, 94, 88, 75, 66, 122, 91, 73, 56, 42, 64, 44, 21, 25,
    90, 43, 41, 77, 73, 63, 56, 92, 77, 66, 47, 67, 48, 53, 36, 20,
    71, 34, 67, 60, 58, 49, 88, 76, 67, 106, 71, 54, 38, 39, 23, 15,
    109, 53, 51, 47, 90, 82, 58, 57, 48, 72, 57, 41, 23, 27, 62, 9,
    86, 42, 40, 37, 70, 64, 52, 43, 70, 55, 42, 25, 29, 18, 11, 11,
    118, 68, 30, 55, 50, 46, 74, 65, 49, 39, 24, 16, 22, 13, 14, 7,
    91, 44, 39, 38, 34, 63, 52, 45, 31, 52, 28, 19, 14, 8, 9, 3,
    123, 60, 58, 53, 47, 43, 32, 22, 37, 24, 17, 12, 15, 10, 2, 1,
    71, 37, 34, 30, 28, 20, 17, 26, 21, 16, 10, 6, 8, 6, 2, 0};
const unsigned char kH15l[] = {
    3, 4, 5, 7, 7, 8, 9, 9, 9, 10, 10, 11, 11, 11, 12, 13,
    4, 3, 5, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11,
    5, 5, 5, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 11, 11, 11,
    6, 6, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11,
    7, 6, 7, 7, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11,
    8, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 11, 11, 11, 12,
    9, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 12, 12,
    9, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 12,
    9, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 12, 12, 12,
    9, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12,
    10, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 12,
    10, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13,
    11, 10, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 13, 13,
    11, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13,
    12, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 12, 13,
    12, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13};
const unsigned short kH16c[] = {
    1, 5, 14, 44, 74, 63, 110, 93, 172, 149, 138, 242, 225, 195, 376, 17,
    3, 4, 12, 20, 35, 62, 53, 47, 83, 75, 68, 119, 201, 107, 207, 9,
    15, 13, 23, 38, 67, 58, 103, 90, 161, 72, 127, 117, 110, 209, 206, 16,
    45, 21, 39, 69, 64, 114, 99, 87, 158, 140, 252, 212, 199, 387, 365, 26,
    75, 36, 68, 65, 115, 101, 179, 164, 155, 264, 246, 226, 395, 382, 362, 9,
    66, 30, 59, 56, 102, 185, 173, 265, 142, 253, 232, 400, 388, 378, 445, 16,
    111, 54, 52, 100, 184, 178, 160, 133, 257, 244, 228, 217, 385, 366, 715, 10,
    98, 48, 91, 88, 165, 157, 148, 261, 248, 407, 397, 372, 380, 889, 884, 8,
    85, 84, 81, 159, 156, 143, 260, 249, 427, 401, 392, 383, 727, 713, 708, 7,
    154, 76, 73, 141, 131, 256, 245, 426, 406, 394, 384, 735, 359, 710, 352, 11,
    139, 129, 67, 125, 247, 233, 229, 219, 393, 743, 737, 720, 885, 882, 439, 4,
    243, 120, 118, 115, 227, 223, 396, 746, 742, 736, 721, 712, 706, 223, 436, 6,
    202, 224, 222, 218, 216, 389, 386, 381, 364, 888, 443, 707, 440, 437, 1728, 4,
    747, 211, 210, 208, 370, 379, 734, 723, 714, 1735, 883, 877, 876, 3459, 865, 2,
    377, 369, 102, 187, 726, 722, 358, 711, 709, 866, 1734, 871, 3458, 870, 434, 0,
    12, 10, 7, 11, 10, 17, 11, 9, 13, 12, 10, 7, 5, 3, 1, 3};
const unsigned char kH16l[] = {
    1, 4, 6, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 9,
    3, 4, 6, 7, 8, 9, 9, 9, 10, 10, 10, 11, 12, 11, 12, 8,
    6, 6, 7, 8, 9, 9, 10, 10, 11, 10, 11, 11, 11, 12, 12, 9,
    8, 7, 8, 9, 9, 10, 10, 10, 11, 11, 12, 12, 12, 13, 13, 10,
    9, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 9,
    9, 8, 9, 9, 10, 11, 11, 12, 11, 12, 12, 13, 13, 13, 14, 10,
    10, 9, 9, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 14, 10,
    10, 9, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 15, 15, 10,
    10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 14, 14, 14, 10,
    11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 14, 13, 14, 13, 11,
    11, 11, 10, 11, 12, 12, 12, 12, 13, 14, 14, 14, 15, 15, 14, 10,
    12, 11, 11, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 13, 14, 11,
    12, 12, 12, 12, 12, 13, 13, 13, 13, 15, 14, 14, 14, 14, 16, 11,
    14, 12, 12, 12, 13, 13, 14, 14, 14, 16, 15, 15, 15, 17, 15, 11,
    13, 13, 11, 12, 14, 14, 13, 14, 14, 15, 16, 15, 17, 15, 14, 11,
    9, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 8};
const unsigned short kH24c[] = {
    15, 13, 46, 80, 146, 262, 248, 434, 426, 669, 653, 649, 621, 517, 1032, 88,
    14, 12, 21, 38, 71, 130, 122, 216, 209, 198, 327, 345, 319, 297, 279, 42,
    47, 22, 41, 74, 68, 128, 120, 221, 207, 194, 182, 340, 315, 295, 541, 18,
    81, 39, 75, 70, 134, 125, 116, 220, 204, 190, 178, 325, 311, 293, 271, 16,
    147, 72, 69, 135, 127, 118, 112, 210, 200, 188, 352, 323, 306, 285, 540, 14,
    263, 66, 129, 126, 119, 114, 214, 202, 192, 180, 341, 317, 301, 281, 262, 12,
    249, 123, 121, 117, 113, 215, 206, 195, 185, 347, 330, 308, 291, 272, 520, 10,
    435, 115, 111, 109, 211, 203, 196, 187, 353, 332, 313, 298, 283, 531, 381, 17,
    427, 212, 208, 205, 201, 193, 186, 177, 169, 320, 303, 286, 268, 514, 377, 16,
    335, 199, 197, 191, 189, 181, 174, 333, 321, 305, 289, 275, 521, 379, 371, 11,
    668, 184, 183, 179, 175, 344, 331, 314, 304, 290, 277, 530, 383, 373, 366, 10,
    652, 346, 171, 168, 164, 318, 309, 299, 287, 276, 263, 513, 375, 368, 362, 6,
    648, 322, 316, 312, 307, 302, 292, 284, 269, 261, 512, 376, 370, 364, 359, 4,
    620, 300, 296, 294, 288, 282, 273, 266, 515, 380, 374, 369, 365, 361, 357, 2,
    1033, 280, 278, 274, 267, 264, 259, 382, 378, 372, 367, 363, 360, 358, 356, 0,
    43, 20, 19, 17, 15, 13, 11, 9, 7, 6, 4, 7, 5, 3, 1, 3};
const unsigned char kH24l[] = {
    4, 4, 6, 7, 8, 9, 9, 10, 10, 11, 11, 11, 11, 11, 12, 9,
    4, 4, 5, 6, 7, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 8,
    6, 5, 6, 7, 7, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 7,
    7, 6, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 7,
    8, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 11, 7,
    9, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 7,
    9, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 7,
    10, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 8,
    10, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 8,
    10, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 8,
    11, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 8,
    11, 10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 8,
    11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8,
    11, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 8,
    12, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 8,
    8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 4};
// The two tables for the run of quadruples of zeros and ones at the top of the spectrum: the code of
// (v, w, x, y) is at [v * 8 + w * 4 + x * 2 + y].
const unsigned short kHAc[] = {1, 5, 4, 5, 6, 5, 4, 4, 7, 3, 6, 0, 7, 2, 3, 1};
const unsigned char kHAl[] = {1, 4, 4, 5, 4, 6, 5, 6, 4, 5, 5, 6, 5, 6, 6, 6};
const unsigned short kHBc[] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
const unsigned char kHBl[] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

// The 32 table numbers a granule can name: which code table each one uses, how wide it is, and how many
// extra bits ("linbits") follow a value of 15 to carry what is above it. Tables 0, 4 and 14 hold nothing.
struct PairTable { const unsigned short* code; const unsigned char* len; int size; int linbits; };
const PairTable kPair[32] = {
    {nullptr, nullptr, 0, 0}, {kH1c, kH1l, 2, 0}, {kH2c, kH2l, 3, 0}, {kH3c, kH3l, 3, 0},
    {nullptr, nullptr, 0, 0}, {kH5c, kH5l, 4, 0}, {kH6c, kH6l, 4, 0}, {kH7c, kH7l, 6, 0},
    {kH8c, kH8l, 6, 0}, {kH9c, kH9l, 6, 0}, {kH10c, kH10l, 8, 0}, {kH11c, kH11l, 8, 0},
    {kH12c, kH12l, 8, 0}, {kH13c, kH13l, 16, 0}, {nullptr, nullptr, 0, 0}, {kH15c, kH15l, 16, 0},
    {kH16c, kH16l, 16, 1}, {kH16c, kH16l, 16, 2}, {kH16c, kH16l, 16, 3}, {kH16c, kH16l, 16, 4},
    {kH16c, kH16l, 16, 6}, {kH16c, kH16l, 16, 8}, {kH16c, kH16l, 16, 10}, {kH16c, kH16l, 16, 13},
    {kH24c, kH24l, 16, 4}, {kH24c, kH24l, 16, 5}, {kH24c, kH24l, 16, 6}, {kH24c, kH24l, 16, 7},
    {kH24c, kH24l, 16, 8}, {kH24c, kH24l, 16, 9}, {kH24c, kH24l, 16, 11}, {kH24c, kH24l, 16, 13}};

// Half of the window of the polyphase filterbank, in units of 1/65536: the standard's table D[i] is
// this, mirrored about i = 256, with the sign turned over in every second run of 64. The analysis
// window C[i] is D[i] / 32.
const int kWindowHalf[257] = {
    0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3, -3, -4, -4, -5, -5, -6, -7, -7,
    -8, -9, -10, -11, -13, -14, -16, -17, -19, -21, -24, -26, -29, -31, -35, -38, -41, -45, -49, -53,
    -58, -63, -68, -73, -79, -85, -91, -97, -104, -111, -117, -125, -132, -139, -147, -154, -161, -169, -176, -183,
    -190, -196, -202, -208, -213, -218, -222, -225, -227, -228, -228, -227, -224, -221, -215, -208, -200, -189, -177, -163,
    -146, -127, -106, -83, -57, -29, 2, 36, 72, 111, 153, 197, 244, 294, 347, 401, 459, 519, 581, 645,
    711, 779, 848, 919, 991, 1064, 1137, 1210, 1283, 1356, 1428, 1498, 1567, 1634, 1698, 1759, 1817, 1870, 1919, 1962,
    2001, 2032, 2057, 2075, 2085, 2087, 2080, 2063, 2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535, 1414, 1280, 1131, 970,
    794, 605, 402, 185, -45, -288, -545, -814, -1095, -1388, -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585, -9727, -9838, -9916, -9959,
    -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134, -6574, -5959, -5288, -4561, -3776, -2935, -2037, -1082,
    -70, 998, 2122, 3300, 4533, 5818, 7154, 8540, 9975, 11455, 12980, 14548, 16155, 17799, 19478, 21189, 22929, 24694, 26482, 28289,
    30112, 31947, 33791, 35640, 37489, 39336, 41176, 43006, 44821, 46617, 48390, 50137, 51853, 53534, 55178, 56778, 58333, 59838, 61289, 62684,
    64019, 65290, 66494, 67629, 68692, 69679, 70590, 71420, 72169, 72835, 73415, 73908, 74313, 74630, 74856, 74992, 75038};

// A code tree: node[2 * i + bit] is the next node, or minus (value + 1) at a leaf, or 0 where no code goes.
struct HuffTree {
    std::vector<short> node;
    void build(const unsigned short* code, const unsigned char* len, int count) {
        node.assign(2, 0);
        for (int v = 0; v < count; ++v) {
            int at = 0;
            for (int b = len[v] - 1; b >= 0; --b) {
                const int bit = (code[v] >> b) & 1;
                if (b == 0) { node[(size_t)(2 * at + bit)] = (short)-(v + 1); break; }
                int next = node[(size_t)(2 * at + bit)];
                if (next <= 0) {
                    next = (int)node.size() / 2;
                    node[(size_t)(2 * at + bit)] = (short)next;
                    node.push_back(0);
                    node.push_back(0);
                }
                at = next;
            }
        }
    }
};

struct Tables {
    HuffTree pair[32];              // by table number; the ones sharing a code table are copies
    HuffTree quadA;
    float pow43[8207];              // n to the power 4/3
    float quarter[4];               // 2 to the power 0, 1/4, 1/2, 3/4
    float window[512];              // D[i]
    float matrix[32][32];           // the synthesis cosines for outputs 17 to 48; the other 32 follow by symmetry
    float imdctLong[4][36][18];     // by block type (2 unused), with the window multiplied in
    float imdctShort[12][6];
    float aliasCs[8], aliasCa[8];
    float isRatio[7][2];            // MPEG-1 intensity stereo: left and right share by position

    Tables() {
        for (int t = 0; t < 32; ++t)
            if (kPair[t].code) pair[t].build(kPair[t].code, kPair[t].len, kPair[t].size * kPair[t].size);
        quadA.build(kHAc, kHAl, 16);
        for (int i = 0; i < 8207; ++i) pow43[i] = (float)std::pow((double)i, 4.0 / 3.0);
        for (int i = 0; i < 4; ++i) quarter[i] = (float)std::pow(2.0, i / 4.0);
        for (int i = 0; i < 512; ++i) {
            const int v = kWindowHalf[i <= 256 ? i : 512 - i];
            window[i] = (float)(((i >> 6) & 1) ? -v : v) / 65536.0f;
        }
        for (int i = 0; i < 32; ++i)
            for (int k = 0; k < 32; ++k) matrix[i][k] = (float)std::cos((16 + 17 + i) * (2 * k + 1) * kPi / 64.0);
        for (int type = 0; type < 4; ++type)
            for (int i = 0; i < 36; ++i) {
                double w = std::sin(kPi / 36.0 * (i + 0.5));
                if (type == 1 && i >= 18) w = i < 24 ? 1.0 : (i < 30 ? std::sin(kPi / 12.0 * (i - 18 + 0.5)) : 0.0);
                if (type == 3 && i < 18) w = i < 6 ? 0.0 : (i < 12 ? std::sin(kPi / 12.0 * (i - 6 + 0.5)) : 1.0);
                for (int k = 0; k < 18; ++k) imdctLong[type][i][k] = (float)(w * std::cos(kPi / 72.0 * (2 * i + 1 + 18) * (2 * k + 1)));
            }
        for (int i = 0; i < 12; ++i)
            for (int k = 0; k < 6; ++k)
                imdctShort[i][k] = (float)(std::sin(kPi / 12.0 * (i + 0.5)) * std::cos(kPi / 24.0 * (2 * i + 1 + 6) * (2 * k + 1)));
        static const double c[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
        for (int i = 0; i < 8; ++i) {
            aliasCs[i] = (float)(1.0 / std::sqrt(1.0 + c[i] * c[i]));
            aliasCa[i] = (float)(c[i] / std::sqrt(1.0 + c[i] * c[i]));
        }
        for (int i = 0; i < 7; ++i) {
            const double r = i == 6 ? 1.0e30 : std::tan(i * kPi / 12.0);
            isRatio[i][0] = (float)(i == 6 ? 1.0 : r / (1.0 + r));
            isRatio[i][1] = (float)(i == 6 ? 0.0 : 1.0 / (1.0 + r));
        }
    }
};
const Tables& tables() { static const Tables t; return t; }

inline float pow2Quarter(const Tables& T, int e4) { return std::ldexp(T.quarter[e4 & 3], e4 >> 2); }      // 2 to the power e4 / 4

inline uint32 be32(const unsigned char* p) { return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | (uint32)p[3]; }

// CRC-16 with polynomial 0x8005, most significant bit first (frame protection), and the reflected form
// of the same polynomial (the checksums in the Info frame).
unsigned crc16Frame(unsigned crc, const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= (unsigned)p[i] << 8;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1);
        crc &= 0xFFFF;
    }
    return crc;
}
unsigned crc16Info(unsigned crc, const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

// ------------------------------------------------------------------ frame headers

struct Header {
    int sr = 0;                 // index into kRates and the band tables
    bool lsf = false;           // MPEG-2 or 2.5: one granule per frame and the shorter side information
    int bitrateIndex = 0;
    bool padding = false, crc = false;
    int mode = 0;               // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
    int modeExt = 0;            // joint stereo: bit 0 intensity, bit 1 mid/side
    int channels = 0, rate = 0;
    int sideBytes = 0;
    int samples = 0;            // per channel per frame
    int frameBytes = 0;         // 0 for free format until the caller has found it
};

bool parseHeader(const unsigned char* p, Header& h) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    const int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;        // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5; layer 1 is Layer III
    const int bitrate = p[2] >> 4, rate = (p[2] >> 2) & 3;
    if (version == 1 || layer != 1 || bitrate == 15 || rate == 3) return false;
    h.lsf = version != 3;
    h.sr = rate + (version == 3 ? 0 : (version == 2 ? 3 : 6));
    h.rate = kRates[h.sr];
    h.bitrateIndex = bitrate;
    h.crc = (p[1] & 1) == 0;
    h.padding = (p[2] & 2) != 0;
    h.mode = p[3] >> 6;
    h.modeExt = h.mode == 1 ? (p[3] >> 4) & 3 : 0;
    h.channels = h.mode == 3 ? 1 : 2;
    h.sideBytes = h.lsf ? (h.channels == 1 ? 9 : 17) : (h.channels == 1 ? 17 : 32);
    h.samples = h.lsf ? 576 : 1152;
    h.frameBytes = bitrate ? (h.lsf ? 72000 : 144000) * kBitrates[h.lsf ? 1 : 0][bitrate] / h.rate + (h.padding ? 1 : 0) : 0;
    return true;
}
// Two headers of one stream agree in version, layer and sample rate; the rest may change from frame to frame.
inline bool sameStream(const unsigned char* a, const unsigned char* b) { return ((a[1] ^ b[1]) & 0xFE) == 0 && ((a[2] ^ b[2]) & 0x0C) == 0; }

// Walks the frames of a stream. Free format frames do not state their size, so it is measured once as
// the distance to the next header of the same kind.
struct FrameWalker {
    const unsigned char* d;
    size_t end;
    int freeBytes = 0;          // size of a free format frame without its padding byte; 0 until measured
    int freeTries = 0;          // how often measuring it has failed (data made to look like free format must not cost hours)

    // Fills h for the frame at pos. False if no acceptable header is there.
    bool at(size_t pos, Header& h, const unsigned char* like = nullptr) {
        if (pos + 4 > end || !parseHeader(d + pos, h)) return false;
        if (like && !sameStream(d + pos, like)) return false;
        if (h.bitrateIndex == 0) {
            if (!freeBytes) {
                if (freeTries >= 200) return false;
                if (!measureFree(pos)) { ++freeTries; return false; }
            }
            h.frameBytes = freeBytes + (h.padding ? 1 : 0);
        }
        return h.frameBytes >= 4 + (h.crc ? 2 : 0) + h.sideBytes;
    }
    bool measureFree(size_t pos) {
        Header first;
        parseHeader(d + pos, first);
        const size_t limit = std::min(end, pos + 8192);
        for (size_t q = pos + 4 + (size_t)first.sideBytes; q + 4 <= limit; ++q) {
            Header n;
            if (d[q] != 0xFF || !parseHeader(d + q, n) || n.bitrateIndex != 0 || !sameStream(d + q, d + pos) || ((d[q + 3] ^ d[pos + 3]) & 0xC0)) continue;
            const int bytes = (int)(q - pos) - (first.padding ? 1 : 0);
            if (bytes < 4 + (first.crc ? 2 : 0) + first.sideBytes) continue;
            // a third header where this size says it should be settles it (or the end of the data)
            const size_t third = q + (size_t)bytes + (n.padding ? 1 : 0);
            Header m;
            if (third + 4 > end || (parseHeader(d + third, m) && m.bitrateIndex == 0 && sameStream(d + third, d + pos))) { freeBytes = bytes; return true; }
        }
        return false;
    }
    // True if frames follow one another from pos: `need` of them, or, when the data ends first, at
    // least `needAtEnd`. Random bytes hold a believable header every few thousand bytes, so one header
    // proves little; two that agree and sit a frame apart are already beyond chance.
    bool chain(size_t pos, const unsigned char* like, int need, int needAtEnd) {
        const int saved = freeBytes;
        Header h;
        const unsigned char* first = d + pos;
        for (int k = 0; k < need; ++k) {
            if (pos + 4 > end) {                        // the stream ends with (or inside) the frame before
                if (k >= needAtEnd) return true;
                break;
            }
            if (!at(pos, h, k ? first : like)) break;
            if (k + 1 == need) return true;
            pos += (size_t)h.frameBytes;
        }
        freeBytes = saved;
        return false;
    }
    // The start of a stream: three frames in a row, or two if no more fit; a single frame only when it is all there is.
    bool startsAt(size_t pos, bool veryStart) { return chain(pos, nullptr, 3, veryStart ? 1 : 2); }
    // The stream going on after damage: frames like the first ones.
    bool resumesAt(size_t pos, const unsigned char* like) { return chain(pos, like, 2, 1); }
};

// An ID3v2 tag at pos: its whole size, or 0.
size_t id3v2Size(const unsigned char* d, size_t size, size_t pos) {
    if (size - pos < 10 || d[pos] != 'I' || d[pos + 1] != 'D' || d[pos + 2] != '3' || d[pos + 3] == 0xFF || d[pos + 4] == 0xFF) return 0;
    if ((d[pos + 6] | d[pos + 7] | d[pos + 8] | d[pos + 9]) & 0x80) return 0;
    const size_t body = ((size_t)d[pos + 6] << 21) | ((size_t)d[pos + 7] << 14) | ((size_t)d[pos + 8] << 7) | (size_t)d[pos + 9];
    return 10 + body + ((d[pos + 5] & 0x10) ? 10 : 0);
}

// ------------------------------------------------------------------ decoder

// Reads bits most significant first from a byte buffer. Past the end it gives zeros; pos keeps counting,
// so the caller can tell, and every loop that reads is bounded by the size of a granule.
struct Bits {
    const unsigned char* p;
    size_t bytes;
    size_t pos = 0;             // in bits
    Bits(const unsigned char* data, size_t n) : p(data), bytes(n) {}
    uint32 peek32() const {
        const size_t b = pos >> 3;
        uint64 v = 0;
        if (b + 5 <= bytes) v = ((uint64)p[b] << 32) | ((uint64)p[b + 1] << 24) | ((uint64)p[b + 2] << 16) | ((uint64)p[b + 3] << 8) | (uint64)p[b + 4];
        else for (size_t k = 0; k < 5; ++k) v = (v << 8) | (b + k < bytes ? p[b + k] : 0u);
        return (uint32)(v >> (8 - (pos & 7)));
    }
    uint32 get(int n) {         // n is 0 to 24
        if (n == 0) return 0;
        const uint32 v = peek32() >> (32 - n);
        pos += (size_t)n;
        return v;
    }
};

struct Granule {
    int part23Length = 0, bigValues = 0, globalGain = 0, scalefacCompress = 0;
    int blockType = 0;
    bool mixed = false;
    int tableSelect[3] = {};
    int subblockGain[3] = {};
    int region0Count = 0, region1Count = 0;
    bool preflag = false, scalefacScale = false, count1Table = false;
    bool valid = true;
    unsigned char sf[40] = {};          // the scalefactors, band by band in the order of the bitstream
    bool sfIsMax[40] = {};              // MPEG-2 intensity stereo: this position is the "not intensity" mark
};

// The bands of a granule in bitstream order: long bands, then for short blocks each band three times,
// once per window.
struct Layout {
    int n = 0;
    int longBands = 0;                  // how many of them are long
    int longLines = 0;                  // and how many lines those cover
    short width[40];
    signed char win[40];                // -1 for a long band
    unsigned char sfb[40];
};
void makeLayout(const Header& h, int blockType, bool mixed, Layout& L) {
    const short* lo = kSfbLong[h.sr];
    const short* sh = kSfbShort[h.sr];
    L.n = L.longBands = L.longLines = 0;
    auto push = [&](int width, int win, int sfb) { L.width[L.n] = (short)width; L.win[L.n] = (signed char)win; L.sfb[L.n] = (unsigned char)sfb; ++L.n; };
    if (blockType != 2) {
        for (int b = 0; b < 22; ++b) push(lo[b + 1] - lo[b], -1, b);
        L.longBands = 22;
        L.longLines = 576;
        return;
    }
    int from = 0;
    if (mixed) {                        // the lowest lines stay long: up to where short band 3 starts
        L.longBands = h.lsf ? 6 : 8;
        for (int b = 0; b < L.longBands; ++b) push(lo[b + 1] - lo[b], -1, b);
        L.longLines = lo[L.longBands];
        from = 3;
    }
    for (int b = from; b < 13; ++b)
        for (int w = 0; w < 3; ++w) push(sh[b + 1] - sh[b], w, b);
}

struct ChannelState {
    float overlap[576] = {};
    float v[1024] = {};
    int voff = 0;
};

struct Decoder {
    const Tables& T = tables();
    std::vector<unsigned char> reservoir;       // main data of earlier frames, then of this one
    ChannelState ch[2];
    int scfsi[2][4] = {};
    Granule gr[2][2];
    Layout layout[2];
    short is[2][576];
    float xr[2][576];
    int nonzero[2] = {};                // lines of each channel that the Huffman data reached

    bool readSideInfo(const unsigned char* p, const Header& h, int& mainDataBegin);
    void readScalefactors(Bits& b, const Header& h, int g, int c);
    void readHuffman(Bits& b, size_t endBit, const Header& h, const Granule& G, int c);
    void requantize(const Header& h, const Granule& G, int c);
    void stereo(const Header& h, int g);
    void hybrid(const Header& h, const Granule& G, int c, float* out);
    void synth(ChannelState& s, const float* sub, float* out);
    void decodeFrame(const unsigned char* frame, const Header& h, float* out[2]);
    void flush(int channels, int samples, float* out[2]);
};

bool Decoder::readSideInfo(const unsigned char* p, const Header& h, int& mainDataBegin) {
    Bits b(p, (size_t)h.sideBytes);
    bool ok = true;
    if (!h.lsf) {
        mainDataBegin = (int)b.get(9);
        b.get(h.channels == 1 ? 5 : 3);
        for (int c = 0; c < h.channels; ++c)
            for (int i = 0; i < 4; ++i) scfsi[c][i] = (int)b.get(1);
    } else {
        mainDataBegin = (int)b.get(8);
        b.get(h.channels == 1 ? 1 : 2);
    }
    for (int g = 0; g < (h.lsf ? 1 : 2); ++g)
        for (int c = 0; c < h.channels; ++c) {
            Granule& G = gr[g][c];
            G = Granule();
            G.part23Length = (int)b.get(12);
            G.bigValues = (int)b.get(9);
            G.globalGain = (int)b.get(8);
            G.scalefacCompress = (int)b.get(h.lsf ? 9 : 4);
            if (b.get(1)) {
                G.blockType = (int)b.get(2);
                G.mixed = b.get(1) != 0;
                for (int i = 0; i < 2; ++i) G.tableSelect[i] = (int)b.get(5);
                for (int i = 0; i < 3; ++i) G.subblockGain[i] = (int)b.get(3);
                if (G.blockType == 0) G.valid = false;          // window switching announced, but no window named
                G.region0Count = (G.blockType == 2 && !G.mixed) ? 8 : 7;
                G.region1Count = 36;
            } else {
                for (int i = 0; i < 3; ++i) G.tableSelect[i] = (int)b.get(5);
                G.region0Count = (int)b.get(4);
                G.region1Count = (int)b.get(3);
            }
            if (!h.lsf) G.preflag = b.get(1) != 0;
            G.scalefacScale = b.get(1) != 0;
            G.count1Table = b.get(1) != 0;
            if (G.bigValues > 288) G.valid = false;
            ok = ok && G.valid;
        }
    return ok;
}

void Decoder::readScalefactors(Bits& b, const Header& h, int g, int c) {
    Granule& G = gr[g][c];
    const Layout& L = layout[c];
    const int kind = G.blockType != 2 ? 0 : (G.mixed ? 2 : 1);
    if (!h.lsf) {
        const int slen1 = kSlen[0][G.scalefacCompress], slen2 = kSlen[1][G.scalefacCompress];
        if (kind == 0) {
            for (int band = 0; band < 21; ++band) {
                const int group = band < 6 ? 0 : (band < 11 ? 1 : (band < 16 ? 2 : 3));
                if (g == 1 && scfsi[c][group] && gr[0][c].blockType != 2) G.sf[band] = gr[0][c].sf[band];       // shared with the first granule
                else G.sf[band] = (unsigned char)b.get(band < 11 ? slen1 : slen2);
            }
        } else {
            for (int i = 0; i < L.n; ++i) {
                if (L.win[i] < 0) G.sf[i] = (unsigned char)b.get(slen1);
                else if (L.sfb[i] < 12) G.sf[i] = (unsigned char)b.get(L.sfb[i] < 6 ? slen1 : slen2);
            }
        }
        return;
    }
    // MPEG-2: scalefac_compress packs four bit widths; a second channel that carries intensity
    // positions instead of scalefactors packs them differently.
    const bool intensity = c == 1 && (h.modeExt & 1);
    int slen[4] = {}, table = 0;
    int sc = G.scalefacCompress;
    if (!intensity) {
        if (sc < 400) { slen[0] = (sc >> 4) / 5; slen[1] = (sc >> 4) % 5; slen[2] = (sc & 15) >> 2; slen[3] = sc & 3; table = 0; }
        else if (sc < 500) { sc -= 400; slen[0] = (sc >> 2) / 5; slen[1] = (sc >> 2) % 5; slen[2] = sc & 3; table = 1; }
        else { sc -= 500; slen[0] = sc / 3; slen[1] = sc % 3; table = 2; G.preflag = true; }
    } else {
        sc >>= 1;
        if (sc < 180) { slen[0] = sc / 36; slen[1] = (sc % 36) / 6; slen[2] = (sc % 36) % 6; table = 3; }
        else if (sc < 244) { sc -= 180; slen[0] = (sc & 63) >> 4; slen[1] = (sc & 15) >> 2; slen[2] = sc & 3; table = 4; }
        else { sc -= 244; slen[0] = sc / 3; slen[1] = sc % 3; table = 5; }
    }
    int i = 0;
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < kLsfGroups[table][kind][k] && i < 40; ++j, ++i) {
            G.sf[i] = (unsigned char)b.get(slen[k]);
            G.sfIsMax[i] = intensity && slen[k] > 0 && G.sf[i] == (1 << slen[k]) - 1;
        }
}

void Decoder::readHuffman(Bits& b, size_t endBit, const Header& h, const Granule& G, int c) {
    short* out = is[c];
    std::memset(out, 0, sizeof is[c]);
    nonzero[c] = 0;
    int region1, region2;
    if (G.blockType == 0) {
        region1 = kSfbLong[h.sr][std::min(G.region0Count + 1, 22)];
        region2 = kSfbLong[h.sr][std::min(G.region0Count + G.region1Count + 2, 22)];
    } else {
        // window switching: the first region is the first 36 lines for short blocks, and the first
        // eight long bands for the start and stop windows; there is no third region
        region1 = G.blockType == 2 ? 3 * kSfbShort[h.sr][3] : kSfbLong[h.sr][8];
        region2 = 576;
    }
    const int bigEnd = std::min(G.bigValues * 2, 576);
    int i = 0, lastPair = -1;
    while (i < bigEnd && b.pos < endBit) {
        const int region = i < region1 ? 0 : (i < region2 ? 1 : 2);
        const int stop = std::min(bigEnd, region == 0 ? region1 : (region == 1 ? region2 : 576));
        const int t = G.tableSelect[region];
        const PairTable& P = kPair[t];
        if (!P.code) { i = stop; continue; }            // table 0: all zeros, no bits (4 and 14 do not exist)
        const short* node = T.pair[t].node.data();
        for (; i < stop && b.pos < endBit; i += 2) {
            uint32 w = b.peek32();
            int at = 0, used = 0;
            do { at = node[2 * at + (int)(w >> 31)]; w <<= 1; ++used; } while (at > 0);
            if (at == 0) { b.pos = endBit; break; }     // not a code of this table
            b.pos += (size_t)used;
            const int v = -at - 1;
            int x = v / P.size, y = v % P.size;
            if (x == 15 && P.linbits) x += (int)b.get(P.linbits);
            if (x && b.get(1)) x = -x;
            if (y == 15 && P.linbits) y += (int)b.get(P.linbits);
            if (y && b.get(1)) y = -y;
            out[i] = (short)x;
            out[i + 1] = (short)y;
            lastPair = i;
        }
    }
    if (b.pos > endBit && lastPair >= 0) out[lastPair] = out[lastPair + 1] = 0;     // that pair ran past the end of the granule's data
    i = bigEnd;
    // the run of quadruples of -1, 0, 1 goes on until the granule's bits are used up
    const short* quad = T.quadA.node.data();
    while (i + 4 <= 576 && b.pos < endBit) {
        int v;
        if (G.count1Table) v = 15 - (int)b.get(4);
        else {
            uint32 w = b.peek32();
            int at = 0, used = 0;
            do { at = quad[2 * at + (int)(w >> 31)]; w <<= 1; ++used; } while (at > 0);
            b.pos += (size_t)used;
            v = -at - 1;
        }
        short q[4];
        for (int k = 0; k < 4; ++k) q[k] = (short)(((v >> (3 - k)) & 1) ? (b.get(1) ? -1 : 1) : 0);
        if (b.pos > endBit) break;
        for (int k = 0; k < 4; ++k) out[i + k] = q[k];
        i += 4;
    }
    nonzero[c] = i;
    b.pos = endBit;
}

void Decoder::requantize(const Header& h, const Granule& G, int c) {
    const Layout& L = layout[c];
    const short* in = is[c];
    float* out = xr[c];
    const int shift = G.scalefacScale ? 4 : 2;         // a scalefactor step is half or a whole power of two; in quarters
    int at = 0;
    for (int band = 0; band < L.n; ++band) {
        const int w = L.width[band];
        if (at >= nonzero[c]) break;
        int e4 = G.globalGain - 210;
        if (L.win[band] < 0) e4 -= shift * (G.sf[band] + (G.preflag ? kPretab[L.sfb[band]] : 0));
        else e4 -= 8 * G.subblockGain[L.win[band]] + shift * G.sf[band];
        const float gain = pow2Quarter(T, e4);
        for (int i = at; i < at + w; ++i) {
            const int q = in[i];
            out[i] = q >= 0 ? T.pow43[q] * gain : -T.pow43[-q] * gain;
        }
        at += w;
    }
    for (; at < 576; ++at) out[at] = 0.0f;
}

void Decoder::stereo(const Header& h, int g) {
    float* l = xr[0];
    float* r = xr[1];
    const bool ms = (h.modeExt & 2) != 0;
    const float k = 0.70710678118654752f;
    if (!(h.modeExt & 1)) {
        if (ms) {
            const int n = std::max(nonzero[0], nonzero[1]);
            for (int i = 0; i < n; ++i) { const float a = l[i], b = r[i]; l[i] = (a + b) * k; r[i] = (a - b) * k; }
            nonzero[0] = nonzero[1] = n;
        }
        return;
    }
    // Intensity stereo: above the last band in which the second channel holds anything, the first
    // channel carries both and the second channel's scalefactors say how to share it out. Short
    // blocks keep that boundary per window.
    const Granule& G = gr[g][1];
    const Layout& L = layout[1];
    int topLong = -1, topShort[3] = {-1, -1, -1};
    {
        int at = 0;
        for (int band = 0; band < L.n; ++band) {
            bool any = false;
            for (int i = at; i < at + L.width[band]; ++i) if (is[1][i]) { any = true; break; }
            if (any) { if (L.win[band] < 0) topLong = L.sfb[band]; else topShort[L.win[band]] = L.sfb[band]; }
            at += L.width[band];
        }
    }
    const bool anyShort = topShort[0] >= 0 || topShort[1] >= 0 || topShort[2] >= 0;
    const float lsfBase = (G.scalefacCompress & 1) ? 0.70710678118654752f : 0.84089641525371454f;
    int at = 0;
    for (int band = 0; band < L.n; ++band) {
        const int w = L.width[band], win = L.win[band], sfb = L.sfb[band];
        const bool above = win < 0 ? (!anyShort && sfb > topLong) : sfb > topShort[win];
        // the last band has no scalefactor of its own and takes the position of the one below it
        const int src = win < 0 ? (sfb == 21 ? band - 1 : band) : (sfb == 12 ? band - 3 : band);
        const int pos = G.sf[src];
        const bool isBand = above && (h.lsf ? !G.sfIsMax[src] : pos < 7);
        if (isBand) {
            float kl, kr;
            if (!h.lsf) { kl = T.isRatio[pos][0]; kr = T.isRatio[pos][1]; }
            else {
                const float f = std::pow(lsfBase, (float)((pos + 1) >> 1));
                kl = (pos & 1) ? f : 1.0f;
                kr = (pos & 1) ? 1.0f : f;
            }
            for (int i = at; i < at + w; ++i) { const float a = l[i]; l[i] = a * kl; r[i] = a * kr; }
        } else if (ms) {
            for (int i = at; i < at + w; ++i) { const float a = l[i], b = r[i]; l[i] = (a + b) * k; r[i] = (a - b) * k; }
        }
        at += w;
    }
    nonzero[0] = nonzero[1] = 576;
}

void Decoder::synth(ChannelState& s, const float* sub, float* out) {
    s.voff = (s.voff - 64) & 1023;
    float* v = s.v + s.voff;
    for (int i = 0; i < 32; ++i) {
        const float* m = T.matrix[i];
        float sum = 0.0f;
        for (int k = 0; k < 32; ++k) sum += m[k] * sub[k];
        v[17 + i] = sum;
    }
    v[16] = 0.0f;
    for (int i = 0; i < 16; ++i) v[i] = -v[32 - i];
    for (int i = 49; i < 64; ++i) v[i] = v[96 - i];
    const float* d = T.window;
    for (int j = 0; j < 32; ++j) {
        float sum = 0.0f;
        for (int i = 0; i < 8; ++i)
            sum += s.v[(s.voff + i * 128 + j) & 1023] * d[i * 64 + j] + s.v[(s.voff + i * 128 + 96 + j) & 1023] * d[i * 64 + 32 + j];
        out[j] = sum;
    }
}

void Decoder::hybrid(const Header& h, const Granule& G, int c, float* out) {
    float* x = xr[c];
    const Layout& L = layout[c];
    // Short blocks arrive band by band with the three windows one after another; the transform wants
    // them line by line with the windows interleaved.
    if (G.blockType == 2) {
        float tmp[576];
        std::memcpy(tmp, x, sizeof tmp);
        int at = L.longLines;
        for (int band = L.longBands; band < L.n; band += 3) {
            const int w = L.width[band];
            for (int win = 0; win < 3; ++win)
                for (int j = 0; j < w; ++j) x[at + 3 * j + win] = tmp[at + win * w + j];
            at += 3 * w;
        }
    }
    // Undo the aliasing between neighbouring subbands (long blocks only).
    const int longSubbands = G.blockType == 2 ? L.longLines / 18 : 32;
    for (int sb = 1; sb < longSubbands; ++sb) {
        float* p = x + 18 * sb;
        for (int i = 0; i < 8; ++i) {
            const float lo = p[-1 - i], hi = p[i];
            p[-1 - i] = lo * T.aliasCs[i] - hi * T.aliasCa[i];
            p[i] = hi * T.aliasCs[i] + lo * T.aliasCa[i];
        }
    }
    ChannelState& S = ch[c];
    float slots[18][32];
    for (int sb = 0; sb < 32; ++sb) {
        const float* in = x + 18 * sb;
        float z[36];
        bool any = false;
        for (int i = 0; i < 18; ++i) if (in[i] != 0.0f) { any = true; break; }
        if (!any) std::memset(z, 0, sizeof z);
        else if (G.blockType == 2 && sb >= longSubbands) {
            std::memset(z, 0, sizeof z);
            for (int win = 0; win < 3; ++win)
                for (int i = 0; i < 12; ++i) {
                    const float* m = T.imdctShort[i];
                    float sum = 0.0f;
                    for (int k = 0; k < 6; ++k) sum += m[k] * in[3 * k + win];
                    z[6 + 6 * win + i] += sum;
                }
        } else {
            // the lowest subbands of a mixed block keep the normal long window, whatever the block type says
            const auto& m = T.imdctLong[(G.blockType == 2 || (G.mixed && sb < 2)) ? 0 : G.blockType];
            for (int i = 0; i < 36; ++i) {
                float sum = 0.0f;
                for (int k = 0; k < 18; ++k) sum += m[i][k] * in[k];
                z[i] = sum;
            }
        }
        float* ov = S.overlap + 18 * sb;
        for (int t = 0; t < 18; ++t) {
            const float v = z[t] + ov[t];
            ov[t] = z[18 + t];
            slots[t][sb] = ((sb & 1) && (t & 1)) ? -v : v;         // every second sample of every second subband is turned over
        }
    }
    for (int t = 0; t < 18; ++t) synth(S, slots[t], out + 32 * t);
}

void Decoder::decodeFrame(const unsigned char* frame, const Header& h, float* out[2]) {
    const unsigned char* side = frame + 4 + (h.crc ? 2 : 0);
    int mainDataBegin = 0;
    bool ok = readSideInfo(side, h, mainDataBegin);
    const unsigned char* mainData = side + h.sideBytes;
    const size_t mainBytes = (size_t)(frame + h.frameBytes - mainData);
    // This frame's data starts mainDataBegin bytes back, in room left over by the frames before it.
    if ((size_t)mainDataBegin > reservoir.size()) ok = false;
    const size_t start = ok ? reservoir.size() - (size_t)mainDataBegin : 0;
    reservoir.insert(reservoir.end(), mainData, mainData + mainBytes);
    Bits b(reservoir.data() + start, reservoir.size() - start);
    const int granules = h.lsf ? 1 : 2;
    for (int g = 0; g < granules; ++g) {
        for (int c = 0; c < h.channels; ++c) {
            const Granule& G = gr[g][c];
            if (!ok) { std::memset(xr[c], 0, sizeof xr[c]); std::memset(is[c], 0, sizeof is[c]); nonzero[c] = 0; makeLayout(h, 0, false, layout[c]); continue; }
            makeLayout(h, G.blockType, G.mixed, layout[c]);
            const size_t end = b.pos + (size_t)G.part23Length;
            readScalefactors(b, h, g, c);
            readHuffman(b, end, h, G, c);
            requantize(h, G, c);
        }
        if (ok && h.channels == 2 && h.modeExt) stereo(h, g);
        for (int c = 0; c < h.channels; ++c) {
            static const Granule plain;
            hybrid(h, ok ? gr[g][c] : plain, c, out[c] + 576 * g);
        }
    }
    if (reservoir.size() > 8192) reservoir.erase(reservoir.begin(), reservoir.end() - 2048);
}

// Runs silence through the filters, to bring out what is still inside them.
void Decoder::flush(int channels, int samples, float* out[2]) {
    Header h;
    static const Granule plain;
    for (int c = 0; c < channels; ++c) {
        makeLayout(h, 0, false, layout[c]);
        for (int at = 0; at < samples; at += 576) {
            std::memset(xr[c], 0, sizeof xr[c]);
            float block[576];
            hybrid(h, plain, c, block);
            std::memcpy(out[c] + at, block, sizeof(float) * (size_t)std::min(576, samples - at));
        }
    }
}

// The Info (or Xing) frame an encoder may put first: a frame with no audio that tells how many frames
// follow and, in its LAME extension, how many samples of delay and padding the encoder added.
struct InfoTag {
    bool present = false, hasFrames = false, hasDelays = false;
    uint32 frames = 0;
    int delay = 0, padding = 0;
};
InfoTag parseInfoTag(const unsigned char* f, const Header& h) {
    InfoTag t;
    const size_t n = (size_t)h.frameBytes;
    // The tag sits where the main data would start. LAME puts it at the same place when the frame says
    // it has a checksum (the two bytes are then not there); an encoder that leaves room for them is read too.
    size_t p = 4 + (size_t)h.sideBytes;
    auto tagAt = [&](size_t at) { return at + 8 <= n && (std::memcmp(f + at, "Xing", 4) == 0 || std::memcmp(f + at, "Info", 4) == 0); };
    if (!tagAt(p) && h.crc && tagAt(p + 2)) p += 2;
    if (!tagAt(p)) {
        t.present = n >= 40 && std::memcmp(f + 36, "VBRI", 4) == 0;          // another encoder's header frame: no audio either
        return t;
    }
    t.present = true;
    const uint32 flags = be32(f + p + 4);
    p += 8;
    if (flags & 1) { if (p + 4 > n) return t; t.frames = be32(f + p); t.hasFrames = true; p += 4; }
    if (flags & 2) p += 4;
    if (flags & 4) p += 100;
    if (flags & 8) p += 4;
    if (p + 24 > n) return t;
    bool named = true;          // the extension starts with the encoder's name
    for (int i = 0; i < 4; ++i) named = named && f[p + (size_t)i] > 0x20 && f[p + (size_t)i] < 0x7F;
    if (!named) return t;
    t.delay = ((int)f[p + 21] << 4) | (f[p + 22] >> 4);
    t.padding = (((int)f[p + 22] & 15) << 8) | (int)f[p + 23];
    t.hasDelays = true;
    return t;
}

constexpr int kDecoderDelay = 529;      // samples the decoder's filters hold back (528) plus one

// Where the audio frames of a file are: after ID3v2 tags and before an ID3v1 tag.
void audioSpan(const unsigned char* d, size_t size, size_t& from, size_t& to) {
    from = 0;
    to = size;
    for (size_t t; (t = id3v2Size(d, size, from)) != 0 && from + t <= size;) from += t;
    if (id3v2Size(d, size, from)) from = size;                   // a tag that claims more than the file holds
    if (to - from >= 128 && std::memcmp(d + to - 128, "TAG", 3) == 0) to -= 128;
}

} // namespace

bool looksLikeMp3(const void* data, size_t size) {
    const unsigned char* d = (const unsigned char*)data;
    if (!d || size < 4) return false;
    size_t from, to;
    audioSpan(d, size, from, to);
    if (from > 0) return !(size - from >= 4 && std::memcmp(d + from, "fLaC", 4) == 0);       // FLAC files may carry an ID3v2 tag too
    FrameWalker w{d, to};
    const size_t limit = std::min(to, (size_t)16384);
    for (size_t pos = 0; pos + 4 <= limit; ++pos)
        if (d[pos] == 0xFF && w.startsAt(pos, pos == 0)) return true;
    return false;
}

namespace {

std::shared_ptr<SampleData> decodeStream(const unsigned char* d, size_t size, std::string* error) {
    auto fail = [&](const char* msg) { if (error) *error = msg; return nullptr; };
    if (!d || size < 4) return fail("not an MP3 file");
    size_t pos, end;
    audioSpan(d, size, pos, end);
    FrameWalker w{d, end};
    // the first frame: the first place from which frames follow one another
    Header h;
    for (const size_t from = pos;; ++pos) {
        if (pos + 4 > end) return fail("not an MP3 file: no MPEG audio frames found");
        if (d[pos] != 0xFF || !w.startsAt(pos, pos == from)) continue;
        w.at(pos, h);
        if (pos + (size_t)h.frameBytes > end) continue;         // not one whole frame
        break;
    }
    const unsigned char* first = d + pos;
    const Header stream = h;
    InfoTag tag = parseInfoTag(d + pos, h);
    if (tag.present) pos += (size_t)h.frameBytes;

    auto s = std::make_shared<SampleData>();
    s->sampleRate = stream.rate;
    s->channels = stream.channels;
    auto dec = std::make_unique<Decoder>();
    // room for the frames to come, going by the size of the first one; not beyond a few minutes' worth,
    // because later frames may be far larger than the first (the vectors grow by themselves after that)
    const size_t guess = std::min<size_t>((end - pos) / (size_t)std::max(stream.frameBytes, 24) + 2, 8192);
    for (int c = 0; c < s->channels; ++c) s->ch[c].reserve(guess * (size_t)stream.samples);
    std::vector<float> buf[2];
    for (auto& v : buf) v.resize(1152);
    uint64 frames = 0;
    bool cut = false;
    while (pos + 4 <= end) {
        if (!w.at(pos, h, first)) {
            // damage, or something that is not audio: look for the place where frames go on
            size_t q = pos + 1;
            while (q + 4 <= end && !(d[q] == 0xFF && w.resumesAt(q, first))) ++q;
            if (q + 4 > end) break;
            pos = q;
            continue;
        }
        if (pos + (size_t)h.frameBytes > end) { cut = true; break; }       // the last frame is not all there
        float* out[2] = {buf[0].data(), buf[1].data()};
        dec->decodeFrame(d + pos, h, out);
        for (int c = 0; c < s->channels; ++c) {
            const float* from = buf[h.channels == 2 ? c : 0].data();
            s->ch[c].insert(s->ch[c].end(), from, from + h.samples);
        }
        pos += (size_t)h.frameBytes;
        ++frames;
    }
    // Trim what the encoder and the decoder added. Without an Info frame only the decoder's share is known.
    const uint64 decoded = frames * (uint64)stream.samples;
    uint64 skip = kDecoderDelay, length;
    if (tag.hasDelays) {
        skip += (uint64)tag.delay;
        const bool whole = !cut && (!tag.hasFrames || frames >= tag.frames);
        const uint64 lead = (uint64)tag.delay + (whole ? (uint64)tag.padding : 0);
        length = decoded > lead ? decoded - lead : 0;
        if (!whole) length = decoded > skip ? decoded - skip : 0;
    } else length = decoded > skip ? decoded - skip : 0;
    if (length > 0 && skip + length > decoded) {            // the end of the audio is still inside the decoder's filters
        const int more = (int)std::min<uint64>(skip + length - decoded, 2304);
        for (auto& v : buf) v.assign((size_t)more, 0.0f);
        float* out[2] = {buf[0].data(), buf[1].data()};
        dec->flush(s->channels, more, out);
        for (int c = 0; c < s->channels; ++c) s->ch[c].insert(s->ch[c].end(), buf[c].begin(), buf[c].end());
    }
    for (int c = 0; c < s->channels; ++c) {
        auto& v = s->ch[c];
        const size_t a = (size_t)std::min<uint64>(skip, v.size());
        const size_t n = (size_t)std::min<uint64>(length, v.size() - a);
        v.erase(v.begin(), v.begin() + (std::ptrdiff_t)a);
        v.resize(n);
        v.shrink_to_fit();
    }
    return s;
}

} // namespace

std::shared_ptr<SampleData> readMp3Memory(const void* data, size_t size, std::string* error) {
    try {
        return decodeStream((const unsigned char*)data, size, error);
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory to decode this MP3 file";
        return nullptr;
    }
}

std::shared_ptr<SampleData> readMp3File(const std::string& utf8Path, std::string* error) {
    std::vector<unsigned char> b;
    if (!readFileBytes(utf8Path, b)) { if (error) *error = "cannot open " + utf8Path; return nullptr; }
    auto s = readMp3Memory(b.data(), b.size(), error);
    if (s) s->path = utf8Path;
    return s;
}

// ------------------------------------------------------------------ encoder

namespace {

struct BitWriter {
    std::vector<unsigned char>& o;
    uint64 acc = 0;
    int n = 0;
    size_t count = 0;
    explicit BitWriter(std::vector<unsigned char>& out) : o(out) {}
    void put(uint32 v, int k) {         // k is 0 to 32 and v has no bits above k
        if (k == 0) return;
        acc = (acc << k) | v;
        n += k;
        count += (size_t)k;
        while (n >= 8) { o.push_back((unsigned char)(acc >> (n - 8))); n -= 8; }
    }
    void align() { if (n) put(0, 8 - n); }
};

constexpr int kEncoderDelay = 528;      // what the analysis filterbank and the transform hold back
constexpr float kInputLimit = 64.0f;     // samples beyond 64 times full scale are clipped there on the way in
constexpr float kOnsetFloor = 0.01f;    // how far the rule against pre-echo may push a threshold down
constexpr int kMaxValue = 8206;         // the largest value the Huffman tables can carry: 15 + 13 extra bits

struct EncTables {
    float window[512];                  // the analysis window C[i]
    float cosine[32][32];               // cos((2k + 1) j pi / 64): the analysis matrix after folding 64 inputs to 32
    unsigned char pairBits[32][256];    // bits to code (x, y), both under 16, with their sign bits, by table number
    float quant[512];                   // 2 ^ (-3 e / 16) for e = index - 380: turns |xr| ^ 0.75 into a quantised value

    EncTables() {
        const Tables& T = tables();
        for (int i = 0; i < 512; ++i) window[i] = T.window[i] / 32.0f;
        for (int k = 0; k < 32; ++k)
            for (int j = 0; j < 32; ++j) cosine[k][j] = (float)std::cos((2 * k + 1) * j * kPi / 64.0);
        for (int t = 0; t < 32; ++t)
            for (int x = 0; x < 16; ++x)
                for (int y = 0; y < 16; ++y) {
                    const PairTable& P = kPair[t];
                    pairBits[t][x * 16 + y] = (P.code && x < P.size && y < P.size) ? (unsigned char)(P.len[x * P.size + y] + (x ? 1 : 0) + (y ? 1 : 0)) : (unsigned char)200;
                }
        for (int i = 0; i < 512; ++i) quant[i] = (float)std::pow(2.0, -0.1875 * (i - 380));
    }
};
const EncTables& encTables() { static const EncTables t; return t; }

// One granule of one channel on its way into the bitstream.
struct GrCh {
    int blockType = 0;
    bool mixed = false;         // the two lowest subbands keep long windows
    Layout L;
    float xr[576];              // the spectrum, in the order it is coded
    float xr34[576];            // |xr| ^ (3/4)
    int ix[576];                // quantised magnitudes
    float energy[40], xmin[40]; // per band: energy, and how much noise may be put there
    float bandMax34[40];
    float pe = 0.0f;            // a guess at the bits it wants
    int sf[40] = {};
    bool fixedSf[40] = {};      // intensity stereo positions: not the quantiser's to change
    int forceCompress = -1;
    int scale = 0, preflag = 0, compress = 0;
    int globalGain = 210, bigValues = 0, count1 = 0, count1Table = 0;
    int tableSelect[3] = {}, region0 = 0, region1 = 0;
    int part2Bits = 0, bits = 0;        // bits = part2_3_length
};

inline bool bandHasSf(const Layout& L, int b) { return L.win[b] < 0 ? L.sfb[b] < 21 : L.sfb[b] < 12; }
inline bool bandInFirstGroup(const Layout& L, int b) { return L.win[b] < 0 ? L.sfb[b] < 11 : L.sfb[b] < 6; }

struct Encoder {
    const Tables& T = tables();
    const EncTables& E = encTables();
    Header h;
    int channels = 1, kbps = 192;
    Mp3EncodeOptions opt;
    int cutoffLine = 576, cutoffHz = 0;
    float athLong[22], athShort[13];
    float spreadLong[22][22], spreadShort[13][13];

    struct Chan {
        float x[512] = {};              // the newest 512 input samples, newest first
        float sb[3][18][32] = {};       // subband samples of the last, this and the next granule
        float subE[3][3] = {};          // for finding attacks: the energy of the input's highs in each third of those granules,
        float subAll[3][3] = {};        // and of all of it
        float prevThr[22] = {}, prevShort[13] = {};      // what the last granule's bands (or its last window's) could hide
        bool prevThrValid = false, prevShortValid = false;
        int lastGain = 210;
    } ch[2];
    int prevMask = 0, prevType = 0;
    double avgPe = -1.0;

    void setup(int srIndex, int nch, int bitrateIndex, const Mp3EncodeOptions& o);
    void analyse(Chan& c, const float* in, size_t len, size_t at, float* out);
    void loadGranule(const float* const* in, size_t len, size_t granule, int slot);
    int attackMask(const Chan& c, int which) const;
    void mdct(const Chan& c, int blockType, bool mixed, float* out);
    void thresholds(GrCh& g, Chan& c);
    float perceptualEntropy(const GrCh& g) const;
    void intensity(GrCh& l, GrCh& r);
    int scalefacBits(GrCh& g) const;
    int regionBits(const int* ix, int a, int b, int& table) const;
    int huffmanBits(GrCh& g, bool optimise) const;
    int quantize(GrCh& g, int gain) const;
    int tryGain(GrCh& g, int gain) const;
    void quantizeGranule(GrCh& g, int maxBits, Chan& c);
    void writeGranule(BitWriter& bw, const GrCh& g) const;
    std::vector<unsigned char> run(const float* const* in, size_t len);
};

double barkOf(double hz) { return 13.0 * std::atan(0.00076 * hz) + 3.5 * std::atan((hz / 7500.0) * (hz / 7500.0)); }
// The threshold of hearing in quiet, in dB SPL.
double quietDb(double hz) {
    const double k = std::max(hz, 20.0) / 1000.0;
    return 3.64 * std::pow(k, -0.8) - 6.5 * std::exp(-0.6 * (k - 3.3) * (k - 3.3)) + 0.001 * k * k * k * k;
}

void Encoder::setup(int srIndex, int nch, int bitrateIndex, const Mp3EncodeOptions& o) {
    opt = o;
    channels = nch;
    h = Header();
    h.sr = srIndex;
    h.rate = kRates[srIndex];
    h.lsf = false;
    h.bitrateIndex = bitrateIndex;
    kbps = kBitrates[0][bitrateIndex];
    h.channels = nch;
    h.sideBytes = nch == 1 ? 17 : 32;
    h.samples = 1152;
    // What is kept of the top of the spectrum: the fewer bits per channel, the less.
    const int perChannel = kbps / nch;
    cutoffHz = perChannel <= 48 ? 15000 : perChannel <= 56 ? 15500 : perChannel <= 64 ? 16000 : perChannel <= 80 ? 17500 :
               perChannel <= 96 ? 19000 : perChannel <= 112 ? 19500 : perChannel <= 128 ? 20000 : 20500;
    cutoffLine = std::min(576, (int)std::lround(cutoffHz * 1152.0 / h.rate));
    cutoffHz = std::min(cutoffHz, h.rate / 2);

    // Masking: how far a band's energy reaches into the bands around it (steeply downwards, less steeply
    // upwards), and the level below which nothing is heard at all. A full-scale sine puts about 1.0 of
    // energy into a granule's spectrum; it is taken to play at 96 dB SPL, and the quiet threshold is
    // then set 20 dB lower still, because audio in a project may yet be turned up.
    auto fill = [&](const short* edge, int n, double linesToHz, float* ath, float* spread, int stride) {
        std::vector<double> z((size_t)n);
        for (int b = 0; b < n; ++b) {
            const double lo = edge[b] * linesToHz, hi = edge[b + 1] * linesToHz;
            z[(size_t)b] = barkOf(0.5 * (lo + hi));
            double q = 1.0e9;
            for (int k = 0; k <= 4; ++k) q = std::min(q, quietDb(lo + (hi - lo) * k / 4.0));
            ath[b] = (float)std::pow(10.0, (q - 96.0 - 20.0) / 10.0);
        }
        for (int b = 0; b < n; ++b)
            for (int m = 0; m < n; ++m) {
                const double dz = z[(size_t)b] - z[(size_t)m];      // the band being masked, minus the masker
                spread[b * stride + m] = (float)std::pow(10.0, -(dz >= 0.0 ? 1.5 * dz : -2.7 * dz));
            }
    };
    fill(kSfbLong[h.sr], 22, h.rate / 1152.0, athLong, &spreadLong[0][0], 22);
    fill(kSfbShort[h.sr], 13, h.rate / 384.0, athShort, &spreadShort[0][0], 13);
}

// Sample i of the input: silence outside it, and what is far out of range or not a number brought back in.
inline float inputAt(const float* in, size_t len, long long i) {
    if (i < 0 || (size_t)i >= len) return 0.0f;
    const float v = in[i];
    if (std::fabs(v) <= kInputLimit) return v;
    return v > 0.0f ? kInputLimit : (v < 0.0f ? -kInputLimit : 0.0f);
}

// The polyphase analysis filterbank: 32 new samples in, one sample of each of the 32 subbands out.
void Encoder::analyse(Chan& c, const float* in, size_t len, size_t at, float* out) {
    std::memmove(c.x + 32, c.x, 480 * sizeof(float));
    for (size_t i = 0; i < 32; ++i) c.x[31 - i] = inputAt(in, len, (long long)(at + i));
    float y[64];
    for (int i = 0; i < 64; ++i) {
        float s = 0.0f;
        for (int j = 0; j < 512; j += 64) s += E.window[i + j] * c.x[i + j];
        y[i] = s;
    }
    // the 64 cosines are symmetric about input 16 and antisymmetric about input 48
    float u[32];
    u[0] = y[16];
    for (int j = 1; j <= 16; ++j) u[j] = y[16 + j] + y[16 - j];
    for (int j = 17; j < 32; ++j) u[j] = y[16 + j] - y[80 - j];
    for (int k = 0; k < 32; ++k) {
        const float* m = E.cosine[k];
        float s = 0.0f;
        for (int j = 0; j < 32; ++j) s += m[j] * u[j];
        out[k] = s;
    }
}

// Fills subband slot `slot` (0 last, 1 this, 2 next) with granule number `granule` of the input.
void Encoder::loadGranule(const float* const* in, size_t len, size_t granule, int slot) {
    for (int c = 0; c < channels; ++c) {
        Chan& C = ch[c];
        for (int t = 0; t < 18; ++t) analyse(C, in[c], len, (granule * 18 + (size_t)t) * 32, C.sb[slot][t]);
        // A subband sample speaks for the input about 240 samples before the newest one that went into
        // it, so a third of a granule (6 subband samples) is these 192 input samples.
        const long long first = (long long)granule * 576 - 240;
        for (int j = 0; j < 3; ++j) {
            float highs = 0.0f, all = 0.0f;
            for (long long i = first + 192 * j; i < first + 192 * (j + 1); ++i) {
                const float a = inputAt(in[c], len, i), d = a - 2.0f * inputAt(in[c], len, i - 1) + inputAt(in[c], len, i - 2);
                highs += d * d;
                all += a * a;
            }
            C.subE[slot][j] = highs;
            C.subAll[slot][j] = all;
        }
    }
}

// Which thirds of a granule hold an attack: a tenfold rise of the highs over what came just before.
// The highs (the input differenced twice) are used because the energy of a bass note alone swells and
// fades within each of its cycles; for the same reason they must be more than a trace of the whole.
int Encoder::attackMask(const Chan& c, int which) const {
    float p1 = c.subE[which - 1][2], p2 = c.subE[which - 1][1];
    int mask = 0;
    for (int j = 0; j < 3; ++j) {
        const float e = c.subE[which][j];
        if (e > 10.0f * std::max(p1, 0.25f * p2) + 1.0e-5f && e > 1.0e-4f * c.subAll[which][j]) mask |= 1 << j;
        p2 = p1;
        p1 = e;
    }
    return mask;
}

// The transform of one granule: 18 old and 18 new samples of each subband become 18 spectral lines
// (or three times 6 for short blocks). Output is in coded order.
void Encoder::mdct(const Chan& c, int blockType, bool mixed, float* out) {
    float x[576];
    for (int sb = 0; sb < 32; ++sb) {
        float in[36];
        for (int t = 0; t < 18; ++t) {
            const bool flip = (sb & 1) && (t & 1);
            in[t] = flip ? -c.sb[0][t][sb] : c.sb[0][t][sb];
            in[18 + t] = flip ? -c.sb[1][t][sb] : c.sb[1][t][sb];
        }
        float* o = x + 18 * sb;
        if (blockType == 2 && !(mixed && sb < 2)) {
            for (int w = 0; w < 3; ++w)
                for (int k = 0; k < 6; ++k) {
                    float s = 0.0f;
                    for (int i = 0; i < 12; ++i) s += T.imdctShort[i][k] * in[6 + 6 * w + i];
                    o[3 * k + w] = s * (1.0f / 3.0f);
                }
        } else {
            const auto& m = T.imdctLong[(mixed && sb < 2) ? 0 : blockType];
            float acc[18] = {};
            for (int i = 0; i < 36; ++i) {
                const float v = in[i];
                for (int k = 0; k < 18; ++k) acc[k] += m[i][k] * v;
            }
            for (int k = 0; k < 18; ++k) o[k] = acc[k] * (1.0f / 9.0f);
        }
    }
    // long blocks: fold the aliasing between neighbouring subbands the way the decoder will unfold it
    const int longSubbands = blockType != 2 ? 32 : (mixed ? 2 : 0);
    for (int sb = 1; sb < longSubbands; ++sb) {
        float* p = x + 18 * sb;
        for (int i = 0; i < 8; ++i) {
            const float lo = p[-1 - i], hi = p[i];
            p[-1 - i] = lo * T.aliasCs[i] + hi * T.aliasCa[i];
            p[i] = hi * T.aliasCs[i] - lo * T.aliasCa[i];
        }
    }
    const int longLines = 18 * longSubbands;
    for (int i = 0; i < 576; ++i) if ((i < longLines ? i : i - i % 3) >= cutoffLine) x[i] = 0.0f;
    std::memcpy(out, x, sizeof x);
    if (blockType != 2) return;
    // short blocks are coded band by band, each band's three windows one after another
    const short* sh = kSfbShort[h.sr];
    for (int b = mixed ? 3 : 0; b < 13; ++b) {
        const int w = sh[b + 1] - sh[b];
        for (int win = 0; win < 3; ++win)
            for (int j = 0; j < w; ++j) out[3 * sh[b] + win * w + j] = x[3 * (sh[b] + j) + win];
    }
}

// Band energies and the noise each band can hide. c is the channel, whose history limits how fast
// the thresholds may rise.
void Encoder::thresholds(GrCh& g, Chan& c) {
    const Layout& L = g.L;
    float mask[40];
    int at = 0;
    for (int b = 0; b < L.n; ++b) {
        const int w = L.width[b];
        float e = 0.0f, peak = 0.0f;
        for (int i = at; i < at + w; ++i) { const float v = g.xr[i] * g.xr[i]; e += v; peak = std::max(peak, v); }
        g.energy[b] = e;
        // A band that is one spectral peak is a tone, and noise shows up beside a tone far more readily
        // than beside noise: 20 dB below a tone, 9 dB below noise. Narrow bands are taken to be tonal.
        float tonal = 1.0f;
        if (w >= 8 && e > 0.0f) tonal = clampT((peak / e - 0.2f) * 2.0f, 0.0f, 1.0f);
        mask[b] = e * std::pow(10.0f, -(0.9f + 1.1f * tonal));
        at += w;
    }
    const bool isLong = g.blockType != 2;
    const int firstShort = L.n > L.longBands ? L.sfb[L.longBands] : 0;         // the short band the short part starts with
    float quiet[40];
    for (int b = 0; b < L.n; ++b) {
        float thr = 0.0f;
        const int win = L.win[b], sfb = L.sfb[b];
        if (win < 0) {
            for (int m = 0; m < L.longBands; ++m) thr = std::max(thr, mask[m] * spreadLong[sfb][m]);
        } else {
            for (int m = firstShort; m < 13; ++m) thr = std::max(thr, mask[L.longBands + (m - firstShort) * 3 + win] * spreadShort[sfb][m]);
        }
        quiet[b] = win < 0 ? athLong[sfb] : athShort[sfb];
        g.xmin[b] = std::max(thr, quiet[b]);
    }
    // Just before an attack the ear has not been deafened yet: a threshold may be at most twice what
    // the block before allowed in the same band (for short blocks, the window before), though never
    // less than a hundredth (20 dB under) of what it would be otherwise. This is what makes the encoder
    // spend bits on the onset instead of letting its noise spread ahead of it.
    if (isLong) {
        for (int b = 0; b < L.n; ++b) {
            const float raw = g.xmin[b];
            if (c.prevThrValid) g.xmin[b] = std::max({quiet[b], kOnsetFloor * raw, std::min(raw, 2.0f * c.prevThr[b])});
            c.prevThr[b] = raw;
        }
        c.prevThrValid = true;
        c.prevShortValid = false;
    } else {
        float before[13] = {};
        bool have = false;
        if (c.prevShortValid) {
            std::memcpy(before, c.prevShort, sizeof before);
            have = true;
        } else if (c.prevThrValid) {
            // from a long block: a short band holds the same frequencies in a third of the time
            const short* lo = kSfbLong[h.sr];
            const short* sh = kSfbShort[h.sr];
            for (int sfb = 0; sfb < 13; ++sfb) {
                float sum = 0.0f;
                for (int m = 0; m < 22; ++m) {
                    const int lines = std::min<int>(lo[m + 1], 3 * sh[sfb + 1]) - std::max<int>(lo[m], 3 * sh[sfb]);
                    if (lines > 0) sum += c.prevThr[m] * (float)lines / (float)(lo[m + 1] - lo[m]);
                }
                before[sfb] = sum / 3.0f;
            }
            have = true;
        }
        for (int win = 0; win < 3; ++win) {
            for (int sfb = firstShort; sfb < 13; ++sfb) {
                const int b = L.longBands + (sfb - firstShort) * 3 + win;
                const float raw = g.xmin[b];
                if (have) g.xmin[b] = std::max({quiet[b], kOnsetFloor * raw, std::min(raw, 2.0f * before[sfb])});
                before[sfb] = raw;
            }
            have = true;
        }
        std::memcpy(c.prevShort, before, sizeof before);
        c.prevShortValid = true;
        c.prevThrValid = false;
    }
    g.pe = perceptualEntropy(g);
}

float Encoder::perceptualEntropy(const GrCh& g) const {
    float pe = 0.0f;
    for (int b = 0; b < g.L.n; ++b)
        if (g.energy[b] > g.xmin[b]) pe += (float)g.L.width[b] * 0.5f * std::log2(g.energy[b] / g.xmin[b]);
    return pe;
}

// Intensity stereo (a test option): above a fixed band the left channel carries the sum of both, the
// right channel carries nothing, and the right channel's scalefactors say how to share the sum out.
void Encoder::intensity(GrCh& l, GrCh& r) {
    const Layout& L = l.L;
    r.forceCompress = 13;                       // three bits for every scalefactor: positions run to 6, and 7 means "not intensity"
    int start[41];
    start[0] = 0;
    for (int b = 0; b < L.n; ++b) start[b + 1] = start[b] + L.width[b];
    auto energies = [&](int b, double& el, double& er) {
        for (int i = start[b]; i < start[b + 1]; ++i) { el += (double)l.xr[i] * l.xr[i]; er += (double)r.xr[i] * r.xr[i]; }
    };
    for (int b = 0; b < L.n; ++b) {
        const bool isLong = L.win[b] < 0;
        if (L.sfb[b] < (isLong ? 12 : 6) || !bandHasSf(L, b)) continue;
        // the last band has no scalefactor and goes by the position of the band below it
        const int above = b + (isLong ? 1 : 3);
        const bool pair = above < L.n && !bandHasSf(L, above);
        double el = 0.0, er = 0.0;
        energies(b, el, er);
        if (pair) energies(above, el, er);
        const int pos = clampT((int)std::lround(std::atan2(std::sqrt(el), std::sqrt(er)) * 12.0 / kPi), 0, 6);
        r.sf[b] = pos;
        r.fixedSf[b] = true;
        const double kl = T.isRatio[pos][0], kr = T.isRatio[pos][1];
        for (int k = b; k <= (pair ? above : b); k += above - b) {
            double bl = 0.0, br = 0.0, bs = 0.0;
            energies(k, bl, br);
            for (int i = start[k]; i < start[k + 1]; ++i) bs += (double)(l.xr[i] + r.xr[i]) * (l.xr[i] + r.xr[i]);
            // the sum, brought to the level that gives the two channels their energy back
            const float gain = bs > 0.0 ? (float)std::min(4.0, std::sqrt((bl + br) / (bs * (kl * kl + kr * kr)))) : 0.0f;
            for (int i = start[k]; i < start[k + 1]; ++i) { l.xr[i] = (l.xr[i] + r.xr[i]) * gain; r.xr[i] = 0.0f; }
        }
    }
}

// How many bits the scalefactors take, choosing the cheapest of the sixteen pairs of widths that holds them.
int Encoder::scalefacBits(GrCh& g) const {
    const Layout& L = g.L;
    int max1 = 0, max2 = 0;
    for (int b = 0; b < L.n; ++b) {
        if (!bandHasSf(L, b)) continue;
        const int v = g.sf[b] - (g.preflag ? kPretab[L.sfb[b]] : 0);
        if (bandInFirstGroup(L, b)) max1 = std::max(max1, v); else max2 = std::max(max2, v);
    }
    const int n1 = g.blockType != 2 ? 11 : (g.mixed ? 17 : 18), n2 = g.blockType != 2 ? 10 : 18;
    if (g.forceCompress >= 0) { g.compress = g.forceCompress; return n1 * kSlen[0][g.compress] + n2 * kSlen[1][g.compress]; }
    int best = -1, bestBits = 1 << 30;
    for (int k = 0; k < 16; ++k) {
        if ((1 << kSlen[0][k]) <= max1 || (1 << kSlen[1][k]) <= max2) continue;
        const int bits = n1 * kSlen[0][k] + n2 * kSlen[1][k];
        if (bits < bestBits) { bestBits = bits; best = k; }
    }
    g.compress = std::max(best, 0);
    return bestBits;
}

// The bits for the pairs in lines a to b with the best table for them.
int Encoder::regionBits(const int* ix, int a, int b, int& table) const {
    table = 0;
    if (a >= b) return 0;
    int mx = 0;
    for (int i = a; i < b; ++i) mx = std::max(mx, ix[i]);
    if (mx == 0) return 0;
    if (mx > 15) {
        // the two families of tables with extra bits: each offers several counts of extra bits
        int t1 = 16, t2 = 24;
        while (t1 < 23 && (1 << kPair[t1].linbits) <= mx - 15) ++t1;
        while (t2 < 31 && (1 << kPair[t2].linbits) <= mx - 15) ++t2;
        int s1 = 0, s2 = 0, escapes = 0;
        for (int i = a; i < b; i += 2) {
            int x = ix[i], y = ix[i + 1];
            if (x > 14) { x = 15; ++escapes; }
            if (y > 14) { y = 15; ++escapes; }
            s1 += E.pairBits[16][x * 16 + y];
            s2 += E.pairBits[24][x * 16 + y];
        }
        s1 += escapes * kPair[t1].linbits;
        s2 += escapes * kPair[t2].linbits;
        table = s2 < s1 ? t2 : t1;
        return std::min(s1, s2);
    }
    static const signed char choice[16][3] = {{0, -1, -1}, {1, -1, -1}, {2, 3, -1}, {5, 6, -1}, {7, 8, 9}, {7, 8, 9}, {10, 11, 12}, {10, 11, 12},
                                              {13, 15, -1}, {13, 15, -1}, {13, 15, -1}, {13, 15, -1}, {13, 15, -1}, {13, 15, -1}, {13, 15, -1}, {13, 15, -1}};
    const signed char* cand = choice[mx];
    int sum[3] = {0, 0, 0};
    const unsigned char* t0 = E.pairBits[cand[0]];
    const unsigned char* t1 = E.pairBits[cand[1] < 0 ? cand[0] : cand[1]];
    const unsigned char* t2 = E.pairBits[cand[2] < 0 ? cand[0] : cand[2]];
    for (int i = a; i < b; i += 2) {
        const int v = ix[i] * 16 + ix[i + 1];
        sum[0] += t0[v];
        sum[1] += t1[v];
        sum[2] += t2[v];
    }
    int best = 0;
    if (cand[1] >= 0 && sum[1] < sum[best]) best = 1;
    if (cand[2] >= 0 && sum[2] < sum[best]) best = 2;
    table = cand[best];
    return sum[best];
}

// Bits for the quantised spectrum, and with them how it is divided: the zeros at the top cost nothing,
// below them a run of quadruples of zeros and ones, below that pairs in up to three regions with a
// table each.
int Encoder::huffmanBits(GrCh& g, bool optimise) const {
    const int* ix = g.ix;
    int i = 576;
    while (i > 1 && !ix[i - 1] && !ix[i - 2]) i -= 2;
    int quads = 0, bitsA = 0, bitsB = 0;
    while (i > 3 && ix[i - 1] <= 1 && ix[i - 2] <= 1 && ix[i - 3] <= 1 && ix[i - 4] <= 1) {
        const int v = ix[i - 4] * 8 + ix[i - 3] * 4 + ix[i - 2] * 2 + ix[i - 1];
        const int ones = ix[i - 4] + ix[i - 3] + ix[i - 2] + ix[i - 1];
        bitsA += kHAl[v] + ones;
        bitsB += 4 + ones;
        i -= 4;
        ++quads;
    }
    g.count1 = quads;
    g.bigValues = i / 2;
    g.count1Table = bitsB < bitsA ? 1 : 0;
    int bits = std::min(bitsA, bitsB);
    const int big = i;
    g.tableSelect[0] = g.tableSelect[1] = g.tableSelect[2] = 0;
    if (g.blockType != 0) {
        // window switching: the first region is fixed at 36 lines and there are only two
        g.region0 = g.blockType == 2 ? 8 : 7;
        g.region1 = 36;
        bits += regionBits(ix, 0, std::min(36, big), g.tableSelect[0]);
        bits += regionBits(ix, std::min(36, big), big, g.tableSelect[1]);
        return bits;
    }
    const short* lo = kSfbLong[h.sr];
    int bands = 0;
    while (lo[bands] < big) ++bands;            // bands that hold pairs
    int r0, r1;
    if (bands <= 2) { r0 = std::max(bands, 1) - 1; r1 = 0; }
    else {
        const int a = clampT((bands + 1) / 3, 1, 16), b = clampT((bands - a) / 2, 1, 8);
        r0 = a - 1;
        r1 = b - 1;
    }
    auto cost = [&](int a, int b, int* tables) {
        const int e0 = std::min<int>(lo[a + 1], big), e1 = std::min<int>(lo[std::min(a + b + 2, 22)], big);
        return regionBits(ix, 0, e0, tables[0]) + regionBits(ix, e0, e1, tables[1]) + regionBits(ix, e1, big, tables[2]);
    };
    int tables[3];
    int best = cost(r0, r1, tables);
    g.region0 = r0;
    g.region1 = r1;
    std::memcpy(g.tableSelect, tables, sizeof tables);
    if (optimise && big > 0) {
        for (int a = std::max(0, r0 - 3); a <= std::min(15, r0 + 3); ++a)
            for (int b = std::max(0, r1 - 3); b <= std::min(7, r1 + 3); ++b) {
                if (a == r0 && b == r1) continue;
                const int c = cost(a, b, tables);
                if (c < best) { best = c; g.region0 = a; g.region1 = b; std::memcpy(g.tableSelect, tables, sizeof tables); }
            }
    }
    return bits + best;
}

// Quantises the spectrum with the given global gain into g.ix. Returns the largest value.
int Encoder::quantize(GrCh& g, int gain) const {
    const Layout& L = g.L;
    const int shift = g.scale ? 4 : 2;
    int at = 0, mx = 0;
    for (int b = 0; b < L.n; ++b) {
        const int w = L.width[b];
        const int e4 = gain - 210 - shift * g.sf[b];
        const float q = E.quant[clampT(e4 + 380, 0, 511)];
        if (g.bandMax34[b] * q + 0.4054f < 1.0f) { for (int i = at; i < at + w; ++i) g.ix[i] = 0; }
        else {
            for (int i = at; i < at + w; ++i) {
                const int v = (int)(g.xr34[i] * q + 0.4054f);
                g.ix[i] = v;
                mx = std::max(mx, v);
            }
        }
        at += w;
    }
    return mx;
}

// Huffman bits at a gain, or a huge number if the values do not fit the tables.
int Encoder::tryGain(GrCh& g, int gain) const {
    if (quantize(g, gain) > kMaxValue) return 1 << 30;
    return huffmanBits(g, false);
}

// Finds the quantisation of one granule of one channel within maxBits: first the finest global step
// that fits, then, where a band's noise is over what the band can hide, that band's scalefactor is
// raised (it is quantised more finely, at the expense of the others) and the step is found again.
void Encoder::quantizeGranule(GrCh& g, int maxBits, Chan& c) {
    const Layout& L = g.L;
    maxBits = clampT(maxBits, 0, 4095);
    float top = 0.0f;
    int at = 0;
    for (int b = 0; b < L.n; ++b) {
        float m = 0.0f;
        for (int i = at; i < at + L.width[b]; ++i) {
            const float a = std::fabs(g.xr[i]);
            const float v = std::sqrt(a * std::sqrt(a));
            g.xr34[i] = v;
            m = std::max(m, v);
        }
        g.bandMax34[b] = m;
        top = std::max(top, m);
        at += L.width[b];
    }
    g.scale = g.preflag = 0;
    bool anyFixed = false;
    for (int b = 0; b < L.n; ++b) { if (!g.fixedSf[b]) g.sf[b] = 0; else anyFixed = true; }
    g.part2Bits = scalefacBits(g);
    if (anyFixed && maxBits < g.part2Bits) {                // no room even for the intensity positions: give them up
        for (int b = 0; b < L.n; ++b) { g.sf[b] = 0; g.fixedSf[b] = false; }
        g.forceCompress = -1;
        anyFixed = false;
        g.part2Bits = scalefacBits(g);
    }
    const bool nothing = top == 0.0f || maxBits <= g.part2Bits;         // nothing to code, or no room for anything

    struct Best { int ix[576]; int sf[40]; int scale = 0, gain = 210; float over = -1.0f; } best;
    int gain = c.lastGain;
    if (nothing) {
        std::memset(best.ix, 0, sizeof best.ix);
        std::memcpy(best.sf, g.sf, sizeof g.sf);
        best.scale = 0;
        best.gain = 210;
    }
    for (int iter = 0; iter < 16 && !nothing; ++iter) {
        g.part2Bits = scalefacBits(g);
        const int budget = maxBits - g.part2Bits;
        // the finest step that fits: bracket it around the last answer, then halve the bracket
        int lowest = 0;
        {
            float m = 0.0f;
            for (int b = 0; b < L.n; ++b) m = std::max(m, g.bandMax34[b] * E.quant[clampT(380 - (g.scale ? 4 : 2) * g.sf[b], 0, 511)]);
            // m * 2^(-3 (gain - 210) / 16) must stay under kMaxValue
            lowest = clampT((int)std::ceil(210.0 + 16.0 / 3.0 * std::log2((double)m / (kMaxValue - 1.0))), 0, 255);
        }
        gain = clampT(gain, lowest, 255);
        int lo, hi;         // lo does not fit (or is below the lowest gain allowed), hi fits
        if (tryGain(g, gain) <= budget) {
            hi = gain;
            lo = lowest - 1;
            for (int step = 2; hi - step > lo; step *= 2) {
                if (tryGain(g, hi - step) <= budget) hi -= step; else { lo = hi - step; break; }
            }
        } else {
            lo = gain;
            hi = 256;
            for (int step = 2; lo + step < hi; step *= 2) {
                if (tryGain(g, lo + step) <= budget) { hi = lo + step; break; } else lo += step;
            }
        }
        while (hi - lo > 1) {
            const int mid = (lo + hi) / 2;
            if (tryGain(g, mid) <= budget) hi = mid; else lo = mid;
        }
        gain = std::min(hi, 255);
        int bits = tryGain(g, gain);
        if (bits > budget) {            // even the coarsest step does not fit: code nothing
            std::memset(g.ix, 0, sizeof g.ix);
            bits = 0;
        }

        // the noise in each band against what it may be
        float over = 0.0f;
        float ratio[40];
        const int shift = g.scale ? 4 : 2;
        at = 0;
        for (int b = 0; b < L.n; ++b) {
            const float step = pow2Quarter(T, gain - 210 - shift * g.sf[b]);
            float noise = 0.0f;
            for (int i = at; i < at + L.width[b]; ++i) {
                const float d = std::fabs(g.xr[i]) - T.pow43[g.ix[i]] * step;
                noise += d * d;
            }
            ratio[b] = noise / g.xmin[b];
            if (bandHasSf(L, b) && !g.fixedSf[b]) over = std::max(over, ratio[b]);
            at += L.width[b];
        }
        if (best.over < 0.0f || over < best.over) {
            std::memcpy(best.ix, g.ix, sizeof g.ix);
            std::memcpy(best.sf, g.sf, sizeof g.sf);
            best.scale = g.scale;
            best.gain = gain;
            best.over = over;
        }
        if (over <= 1.0f) break;

        // raise the scalefactors of the bands that are over
        bool changed = false, wantMore = false;
        for (int b = 0; b < L.n; ++b) {
            if (!bandHasSf(L, b) || g.fixedSf[b] || ratio[b] <= 1.0f) continue;
            const int limit = g.forceCompress >= 0 ? 7 : (bandInFirstGroup(L, b) ? 15 : 7);
            // one step of a scalefactor takes about 2.3 dB (or 4.5 dB) off the band's noise
            const int steps = clampT((int)std::lround(std::log2(ratio[b]) / (g.scale ? 1.5f : 0.75f)), 1, 3);
            const int want = g.sf[b] + steps;
            if (want > limit) wantMore = true;
            const int now = std::min(want, limit);
            if (now != g.sf[b]) { g.sf[b] = now; changed = true; }
        }
        if (wantMore && !g.scale) {             // out of range: make every scalefactor step twice as large
            g.scale = 1;
            for (int b = 0; b < L.n; ++b) if (bandHasSf(L, b) && !g.fixedSf[b]) g.sf[b] = (g.sf[b] + 1) / 2;
            changed = true;
        }
        if (!changed) break;
        bool all = true;
        for (int b = 0; b < L.n; ++b) if (bandHasSf(L, b) && !g.fixedSf[b] && g.sf[b] == 0) all = false;
        if (all) break;             // every band raised: that is only a finer global step, which there are no bits for
    }
    std::memcpy(g.ix, best.ix, sizeof g.ix);
    std::memcpy(g.sf, best.sf, sizeof g.sf);
    g.scale = best.scale;
    g.globalGain = best.gain;
    if (!nothing) c.lastGain = best.gain;

    if (anyFixed) {
        // Intensity positions only count above the last band in which this channel holds anything.
        // Empty bands below the intended boundary would be taken for intensity too, so mark them "not".
        bool live[3] = {true, true, true};
        at = 576;
        for (int b = L.n - 1; b >= 0; --b) {
            at -= L.width[b];
            const int w = L.win[b] < 0 ? 0 : L.win[b];
            bool any = false;
            for (int i = at; i < at + L.width[b]; ++i) if (g.ix[i]) { any = true; break; }
            if (L.win[b] < 0) { if (any) live[0] = live[1] = live[2] = false; if (live[0] && !g.fixedSf[b] && bandHasSf(L, b)) g.sf[b] = 7; }
            else { if (any) live[w] = false; if (live[w] && !g.fixedSf[b] && bandHasSf(L, b)) g.sf[b] = 7; }
        }
    }
    // the same scalefactors may be cheaper to write with the standard high-frequency boost switched on
    if (g.blockType != 2 && g.forceCompress < 0) {
        bool can = true, useful = false;
        for (int b = 11; b < 21; ++b) { if (g.sf[b] < kPretab[b]) can = false; if (g.sf[b]) useful = true; }
        const int plain = scalefacBits(g);
        if (can && useful) {
            g.preflag = 1;
            if (scalefacBits(g) >= plain) g.preflag = 0;
        }
    }
    g.part2Bits = scalefacBits(g);
    g.bits = g.part2Bits + huffmanBits(g, true);
}

void Encoder::writeGranule(BitWriter& bw, const GrCh& g) const {
    const Layout& L = g.L;
    const int slen1 = kSlen[0][g.compress], slen2 = kSlen[1][g.compress];
    for (int b = 0; b < L.n; ++b)
        if (bandHasSf(L, b)) bw.put((uint32)(g.sf[b] - (g.preflag ? kPretab[L.sfb[b]] : 0)), bandInFirstGroup(L, b) ? slen1 : slen2);
    const int big = g.bigValues * 2;
    int region1 = 36, region2 = 576;
    if (g.blockType == 0) {
        region1 = kSfbLong[h.sr][g.region0 + 1];
        region2 = kSfbLong[h.sr][std::min(g.region0 + g.region1 + 2, 22)];
    }
    for (int i = 0; i < big; i += 2) {
        const int t = g.tableSelect[i < region1 ? 0 : (i < region2 ? 1 : 2)];
        const PairTable& P = kPair[t];
        if (!P.code) continue;
        const int x = g.ix[i], y = g.ix[i + 1];
        const int cx = P.linbits && x > 14 ? 15 : x, cy = P.linbits && y > 14 ? 15 : y;
        bw.put(P.code[cx * P.size + cy], P.len[cx * P.size + cy]);
        if (P.linbits && x > 14) bw.put((uint32)(x - 15), P.linbits);
        if (x) bw.put(g.xr[i] < 0.0f ? 1u : 0u, 1);
        if (P.linbits && y > 14) bw.put((uint32)(y - 15), P.linbits);
        if (y) bw.put(g.xr[i + 1] < 0.0f ? 1u : 0u, 1);
    }
    for (int i = big; i < big + 4 * g.count1; i += 4) {
        const int v = g.ix[i] * 8 + g.ix[i + 1] * 4 + g.ix[i + 2] * 2 + g.ix[i + 3];
        if (g.count1Table) bw.put((uint32)(15 - v), 4); else bw.put(kHAc[v], kHAl[v]);
        for (int k = 0; k < 4; ++k) if (g.ix[i + k]) bw.put(g.xr[i + k] < 0.0f ? 1u : 0u, 1);
    }
}

std::vector<unsigned char> Encoder::run(const float* const* in, size_t len) {
    // As many frames as hold the audio and what the filterbanks of encoder and decoder delay it by; and
    // never fewer than two, because some programs (ffmpeg is one) will not open a file with less.
    const size_t frames = std::max<size_t>(2, (len + (size_t)kEncoderDelay + kDecoderDelay + 1151) / 1152);
    const int crcBytes = opt.crc ? 2 : 0;
    // Bytes of the main data of a frame may sit in earlier frames that had room to spare: the bit
    // reservoir. It is limited by the 9-bit pointer and by the buffer a decoder must have (7680 bits).
    const int frameBits = 8 * (144000 * kbps / h.rate + 1);
    const int resvMax = clampT((7680 - frameBits) / 8, 0, 511);

    struct FrameOut { unsigned char head[4 + 2 + 32]; int headBytes; int capacity; };
    std::vector<FrameOut> outFrames;
    outFrames.reserve(frames);
    std::vector<unsigned char> mainStream;
    mainStream.reserve(frames * (size_t)(frameBits / 8));
    size_t capacitySoFar = 0;           // main data room in the frames written so far
    int slotRemainder = 0;

    const bool stereo = channels == 2;
    const int mode = !stereo ? 3 : (opt.stereo == Mp3EncodeOptions::LeftRight ? 0 : (opt.stereo == Mp3EncodeOptions::Dual ? 2 : 1));

    loadGranule(in, len, 0, 1);
    loadGranule(in, len, 1, 2);
    auto g = std::make_unique<GrCh[]>(4);           // [granule * 2 + channel]
    auto alt = std::make_unique<GrCh[]>(4);         // the same as mid and side
    std::vector<unsigned char> frameMain;

    for (size_t f = 0; f < frames; ++f) {
        // ---- the spectra of the frame's two granules
        for (int gr = 0; gr < 2; ++gr) {
            int maskNow = 0, maskNext = 0;
            if (opt.shortBlocks)
                for (int c = 0; c < channels; ++c) { maskNow |= attackMask(ch[c], 1); maskNext |= attackMask(ch[c], 2); }
            // An attack must not sit late in a long window, or the quantisation noise spreads back over
            // the quiet before it. Short blocks for the granule with the attack and the one whose windows reach it.
            const bool needNow = (maskNow & 3) || (prevMask & 6), needNext = (maskNext & 3) || (maskNow & 6);
            int type;
            if (needNow) type = 2;
            else if (prevType == 2) type = needNext ? 2 : 3;
            else type = needNext ? 1 : 0;
            for (int c = 0; c < channels; ++c) {
                GrCh& G = g[gr * 2 + c];
                G = GrCh();
                G.blockType = type;
                G.mixed = opt.mixedBlocks && type != 0;
                makeLayout(h, type, G.mixed, G.L);
                mdct(ch[c], type, G.mixed, G.xr);
            }
            prevMask = maskNow;
            prevType = type;
            for (int c = 0; c < channels; ++c) {
                std::memcpy(ch[c].sb[0], ch[c].sb[1], sizeof ch[c].sb[0]);
                std::memcpy(ch[c].sb[1], ch[c].sb[2], sizeof ch[c].sb[0]);
                std::memcpy(ch[c].subE[0], ch[c].subE[1], sizeof ch[c].subE[0]);
                std::memcpy(ch[c].subE[1], ch[c].subE[2], sizeof ch[c].subE[0]);
                std::memcpy(ch[c].subAll[0], ch[c].subAll[1], sizeof ch[c].subAll[0]);
                std::memcpy(ch[c].subAll[1], ch[c].subAll[2], sizeof ch[c].subAll[0]);
            }
            loadGranule(in, len, f * 2 + (size_t)gr + 2, 2);
        }

        // ---- stereo: as left and right, or as sum and difference when that takes fewer bits
        bool ms = false;
        const bool is = stereo && opt.stereo == Mp3EncodeOptions::Intensity;
        if (is) for (int gr = 0; gr < 2; ++gr) intensity(g[gr * 2], g[gr * 2 + 1]);
        float peTotal = 0.0f;
        for (int gr = 0; gr < 2; ++gr)
            for (int c = 0; c < channels; ++c) { thresholds(g[gr * 2 + c], ch[c]); peTotal += g[gr * 2 + c].pe; }
        if (stereo && (opt.stereo == Mp3EncodeOptions::Auto || opt.stereo == Mp3EncodeOptions::MidSide)) {
            float peMs = 0.0f;
            for (int gr = 0; gr < 2; ++gr) {
                GrCh& L = g[gr * 2];
                GrCh& R = g[gr * 2 + 1];
                GrCh& M = alt[gr * 2];
                GrCh& S = alt[gr * 2 + 1];
                M = L;
                S = R;
                for (int i = 0; i < 576; ++i) {
                    M.xr[i] = (L.xr[i] + R.xr[i]) * 0.70710678f;
                    S.xr[i] = (L.xr[i] - R.xr[i]) * 0.70710678f;
                }
                int at = 0;
                for (int b = 0; b < L.L.n; ++b) {
                    float em = 0.0f, es = 0.0f;
                    for (int i = at; i < at + L.L.width[b]; ++i) { em += M.xr[i] * M.xr[i]; es += S.xr[i] * S.xr[i]; }
                    M.energy[b] = em;
                    S.energy[b] = es;
                    // noise in mid and side lands in both left and right: neither may get more than the stricter allows
                    M.xmin[b] = S.xmin[b] = std::min(L.xmin[b], R.xmin[b]);
                    at += L.L.width[b];
                }
                M.pe = perceptualEntropy(M);
                S.pe = perceptualEntropy(S);
                peMs += M.pe + S.pe;
            }
            ms = opt.stereo == Mp3EncodeOptions::MidSide || peMs < peTotal;
            if (ms) { std::swap(g, alt); peTotal = peMs; }
        }

        // ---- bits: the frame's own, plus or minus the reservoir
        int padding = 0;
        slotRemainder += 144000 * kbps % h.rate;
        if (slotRemainder >= h.rate) { slotRemainder -= h.rate; padding = 1; }
        const int frameBytes = 144000 * kbps / h.rate + padding;
        const int capacity = frameBytes - 4 - crcBytes - h.sideBytes;
        const int reservoir = (int)(capacitySoFar - mainStream.size());         // bytes
        const int meanBits = 8 * capacity, resvBits = 8 * reservoir;
        if (avgPe < 0.0) avgPe = peTotal;
        const double demand = avgPe > 1.0 ? peTotal / avgPe : 1.0;
        avgPe += 0.15 * (peTotal - avgPe);
        double target = meanBits;
        if (demand > 1.0) target += std::min(0.85 * resvBits, meanBits * 0.75 * (demand - 1.0));
        else target -= std::min((double)(8 * resvMax - resvBits), meanBits * 0.2 * (1.0 - demand));
        target = clampT(target, (double)std::max(0, meanBits + resvBits - 8 * resvMax), (double)(meanBits + resvBits));
        int remaining = (int)target, used = 0;
        float weight[4], weightLeft = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const bool there = (k & 1) < channels;
            weight[k] = there ? g[k].pe + 0.05f * peTotal + 1.0f : 0.0f;
            weightLeft += weight[k];
        }
        for (int gr = 0; gr < 2; ++gr)
            for (int c = 0; c < channels; ++c) {
                const int k = gr * 2 + c;
                int share = (int)((double)remaining * weight[k] / std::max(weightLeft, 1.0e-9f));
                if (g[k].forceCompress >= 0) share = std::max(share, std::min(remaining, 160));         // room for the intensity positions
                quantizeGranule(g[k], share, ch[c]);
                remaining -= g[k].bits;
                used += g[k].bits;
                weightLeft -= weight[k];
            }

        // ---- the frame: header, side information, and its main data into the stream of main data
        const int mainDataBegin = reservoir;
        int usedBytes = (used + 7) / 8;
        if (reservoir + capacity - usedBytes > resvMax) usedBytes = reservoir + capacity - resvMax;         // more than may be kept: stuff
        FrameOut fo;
        fo.capacity = capacity;
        fo.head[0] = 0xFF;
        fo.head[1] = (unsigned char)(0xFA | (opt.crc ? 0 : 1));
        fo.head[2] = (unsigned char)((h.bitrateIndex << 4) | (h.sr << 2) | (padding << 1));
        const int modeExt = mode == 1 ? ((ms ? 2 : 0) | (is ? 1 : 0)) : 0;
        fo.head[3] = (unsigned char)((mode << 6) | (modeExt << 4) | 0x04);
        std::vector<unsigned char> side;
        BitWriter sw(side);
        sw.put((uint32)mainDataBegin, 9);
        sw.put(0, stereo ? 3 : 5);
        for (int c = 0; c < channels; ++c) sw.put(0, 4);
        for (int gr = 0; gr < 2; ++gr)
            for (int c = 0; c < channels; ++c) {
                const GrCh& G = g[gr * 2 + c];
                sw.put((uint32)G.bits, 12);
                sw.put((uint32)G.bigValues, 9);
                sw.put((uint32)G.globalGain, 8);
                sw.put((uint32)G.compress, 4);
                if (G.blockType != 0) {
                    sw.put(1, 1);
                    sw.put((uint32)G.blockType, 2);
                    sw.put(G.mixed ? 1u : 0u, 1);
                    sw.put((uint32)G.tableSelect[0], 5);
                    sw.put((uint32)G.tableSelect[1], 5);
                    sw.put(0, 9);
                } else {
                    sw.put(0, 1);
                    for (int k = 0; k < 3; ++k) sw.put((uint32)G.tableSelect[k], 5);
                    sw.put((uint32)G.region0, 4);
                    sw.put((uint32)G.region1, 3);
                }
                sw.put((uint32)G.preflag, 1);
                sw.put((uint32)G.scale, 1);
                sw.put((uint32)G.count1Table, 1);
            }
        sw.align();
        int at = 4;
        if (opt.crc) {
            unsigned crc = crc16Frame(0xFFFF, fo.head + 2, 2);
            crc = crc16Frame(crc, side.data(), side.size());
            fo.head[at++] = (unsigned char)(crc >> 8);
            fo.head[at++] = (unsigned char)(crc & 255);
        }
        std::memcpy(fo.head + at, side.data(), side.size());
        fo.headBytes = at + (int)side.size();
        outFrames.push_back(fo);

        frameMain.clear();
        BitWriter mw(frameMain);
        for (int gr = 0; gr < 2; ++gr)
            for (int c = 0; c < channels; ++c) writeGranule(mw, g[gr * 2 + c]);
        mw.align();
        frameMain.resize((size_t)usedBytes, 0);
        mainStream.insert(mainStream.end(), frameMain.begin(), frameMain.end());
        capacitySoFar += (size_t)capacity;
    }
    mainStream.resize(capacitySoFar, 0);

    // ---- the file: an Info frame, then each frame's header and side information followed by its share of the main data
    std::vector<unsigned char> o;
    const size_t infoBytes = opt.infoFrame ? (size_t)(144000 * kbps / h.rate) : 0;
    o.reserve(infoBytes + capacitySoFar + frames * 38);
    o.resize(infoBytes, 0);
    size_t mainAt = 0;
    for (const FrameOut& fo : outFrames) {
        o.insert(o.end(), fo.head, fo.head + fo.headBytes);
        o.insert(o.end(), mainStream.begin() + (std::ptrdiff_t)mainAt, mainStream.begin() + (std::ptrdiff_t)(mainAt + (size_t)fo.capacity));
        mainAt += (size_t)fo.capacity;
    }
    if (opt.infoFrame) {
        unsigned char* p = o.data();
        p[0] = 0xFF;
        p[1] = 0xFB;
        p[2] = (unsigned char)((h.bitrateIndex << 4) | (h.sr << 2));
        p[3] = (unsigned char)((mode << 6) | 0x04);
        unsigned char* t = p + 4 + h.sideBytes;
        auto be = [](unsigned char* q, uint32 v) { q[0] = (unsigned char)(v >> 24); q[1] = (unsigned char)(v >> 16); q[2] = (unsigned char)(v >> 8); q[3] = (unsigned char)v; };
        std::memcpy(t, "Info", 4);
        be(t + 4, 0x0F);                                    // frames, bytes, seek table and quality follow
        be(t + 8, (uint32)frames);
        be(t + 12, (uint32)o.size());
        for (int i = 0; i < 100; ++i) t[16 + i] = (unsigned char)(i * 256 / 100);     // constant bitrate: time maps straight to position
        be(t + 116, 0);
        // The extension LAME introduced, which decoders look for under that name: this is where the
        // delay and padding are, so that the decoded length comes out exact.
        unsigned char* x = t + 120;
        std::memcpy(x, "LAMEPULP1", 9);
        x[9] = 0x01;                                        // tag revision 0, constant bitrate
        x[10] = (unsigned char)std::min(255, cutoffHz / 100);
        x[20] = (unsigned char)std::min(255, kbps);
        const size_t padding = frames * 1152 - len - (size_t)kEncoderDelay;
        x[21] = (unsigned char)(kEncoderDelay >> 4);
        x[22] = (unsigned char)(((kEncoderDelay & 15) << 4) | ((padding >> 8) & 15));
        x[23] = (unsigned char)(padding & 255);
        const int stereoCode = mode == 3 ? 0 : (mode == 0 ? 1 : (mode == 2 ? 2 : 3));
        x[24] = (unsigned char)(((h.rate == 48000 ? 2 : (h.rate == 44100 ? 1 : 0)) << 6) | (stereoCode << 2));
        be(x + 28, (uint32)o.size());
        const unsigned music = crc16Info(0, o.data() + infoBytes, o.size() - infoBytes);
        x[32] = (unsigned char)(music >> 8);
        x[33] = (unsigned char)(music & 255);
        const unsigned tagCrc = crc16Info(0, p, (size_t)(x + 34 - p));
        x[34] = (unsigned char)(tagCrc >> 8);
        x[35] = (unsigned char)(tagCrc & 255);
    }
    return o;
}

} // namespace

std::vector<unsigned char> encodeMp3(const SampleData& s, const Mp3EncodeOptions& options) {
    // the nearest bitrate MPEG-1 Layer III has between 96 and 320 kbps
    int bitrateIndex = 7;
    for (int i = 7; i < 15; ++i) if (std::abs(kBitrates[0][i] - options.kbps) < std::abs(kBitrates[0][bitrateIndex] - options.kbps)) bitrateIndex = i;
    const int channels = s.channels > 1 ? 2 : 1;
    const long rate = s.sampleRate >= 1.0 ? std::lround(std::min(s.sampleRate, 1.0e7)) : 44100;
    const SampleData* src = &s;
    std::shared_ptr<SampleData> resampled;
    int srIndex = rate == 44100 ? 0 : (rate == 48000 ? 1 : (rate == 32000 ? 2 : -1));
    if (srIndex < 0) {
        // 48 kHz for the rates that divide into it or are multiples of it, and for everything above; else 44.1 kHz
        const bool family48 = rate % 4000 == 0 || rate > 46000;
        const bool family44 = rate % 11025 == 0;
        srIndex = family48 && !family44 ? 1 : 0;
        resampled = stretch::resample(s, (double)kRates[srIndex] / (double)rate);
        src = resampled.get();
    }
    const size_t len = src->ch[0].size();
    std::vector<float> second;          // a stereo sample whose right channel is shorter than its left
    const float* in[2] = {src->ch[0].data(), src->ch[0].data()};
    if (channels == 2) {
        if (src->ch[1].size() >= len) in[1] = src->ch[1].data();
        else { second = src->ch[1]; second.resize(len, 0.0f); in[1] = second.data(); }
    }
    auto enc = std::make_unique<Encoder>();
    enc->setup(srIndex, channels, bitrateIndex, options);
    return enc->run(in, len);
}

std::vector<unsigned char> encodeMp3(const SampleData& s, int kbps) {
    Mp3EncodeOptions o;
    o.kbps = kbps;
    return encodeMp3(s, o);
}

bool writeMp3File(const std::string& utf8Path, const SampleData& s, int kbps, std::string* error) {
    const auto bytes = encodeMp3(s, kbps);
    if (!writeFileBytes(utf8Path, bytes.data(), bytes.size())) { if (error) *error = "cannot write " + utf8Path; return false; }
    return true;
}

} // namespace pulp
