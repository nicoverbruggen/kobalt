#pragma once
#include <QtGlobal>

namespace kobalt::reader::illustrations {
// Text and positions from the original layout.html sample: Libron at 46 px,
// 140% line spacing, on a 1072 x 1448 page. Text is from Alice's Adventures
// in Wonderland, chapter I. Shared lines retain their original word positions.
constexpr int fontPixels = 46;
struct Word { quint16 text; qint16 x; bool italic; };
struct Line { const Word *words; quint8 count; qint16 baseline; };
struct Page { const Line *lines; quint8 count; };
struct Band { qint16 top, bottom; };
struct Bands { const Band *data; quint8 count; };
struct Option { quint8 left, right; Bands bands; };
struct Sample { const char *key; const Option *options; quint8 count, page; bool differs; };

constexpr const char *texts[] = {
    "she ",
    "thought ",
    "it ",
    "over ",
    "afterwards, ",
    "occurred ",
    "to ",
    "her ",
    "that ",
    "ought ",
    "have ",
    "wondered ",
    "at ",
    "this, ",
    "but ",
    "the ",
    "time ",
    "all ",
    "seemed ",
    "quite ",
    "natural); ",
    "when ",
    "Rabbit ",
    "actually ",
    "took ",
    "a ",
    "watch ",
    "out ",
    "of ",
    "its ",
    "waistcoat-pocket",
    ", ",
    "and ",
    "looked ",
    "it, ",
    "then ",
    "hurried ",
    "on, ",
    "Alice ",
    "started ",
    "feet, ",
    "for ",
    "flashed ",
    "across ",
    "mind ",
    "had ",
    "never ",
    "before ",
    "seen ",
    "rabbit ",
    "with ",
    "either ",
    "waistcoat-pocket, ",
    "or ",
    "take ",
    "burning ",
    "curiosity, ",
    "ran ",
    "field ",
    "after ",
    "fortunately ",
    "was ",
    "just ",
    "in ",
    "see ",
    "pop ",
    "down ",
    "large ",
    "rabbit-hole ",
    "under ",
    "hedge.",
    "In ",
    "another ",
    "moment ",
    "went ",
    "once ",
    "considering ",
    "how ",
    "world ",
    "get ",
    "again.",
    "The ",
    "straight ",
    "on ",
    "like ",
    "tunnel ",
    "some ",
    "way, ",
    "dipped ",
    "suddenly ",
    "down, ",
    "so ",
    "not ",
    "think ",
    "about ",
    "stopping ",
    "herself ",
    "found ",
    "falling ",
    "very ",
    "deep ",
    "well.",
    "Either ",
    "well ",
    "deep, ",
    "fell ",
    "slowly, ",
    "plenty ",
    "as ",
    "look ",
    "wonder ",
    "what ",
    "going ",
    "happen ",
    "next. ",
    "First, ",
    "tried ",
    "make ",
    "coming ",
    "to, ",
    "too ",
    "dark ",
    "anything; ",
    "sides ",
    "well, ",
    "noticed ",
    "they ",
    "were ",
    "filled ",
    "cupboards ",
    "book-shelves; ",
    "here ",
    "there ",
    "saw ",
    "maps ",
    "pictures ",
    "hung ",
    "upon ",
    "pegs. ",
    "She ",
    "jar ",
    "from ",
    "one ",
    "shelves ",
    "passed; ",
    "labelled ",
    "“ORANGE ",
    "MARMALADE”, ",
    "great ",
    "disappointment ",
    "empty: ",
    "did ",
    "drop ",
    "fear ",
    "killing ",
    "somebody ",
    "underneath, ",
    "managed ",
    "put ",
    "into ",
    "past ",
    "it.",
    "“Well!” ",
    "herself, ",
    "“after ",
    "such ",
    "fall ",
    "I ",
    "shall ",
    "nothing ",
    "tumbling ",
    "stairs! ",
    "How ",
    "brave ",
    "they’ll ",
    "me ",
    "home! ",
    "Why, ",
    "wouldn’t ",
    "say ",
    "anything ",
    "even ",
    "if ",
    "off ",
    "top ",
    "house!” ",
    "(Which ",
    "likely ",
    "true.)",
    "Down, ",
    "down. ",
    "Would ",
    "never",
    " ",
    "come ",
    "an ",
    "end? ",
    "“I ",
    "many ",
    "miles ",
    "I’ve ",
    "fallen ",
    "by ",
    "this ",
    "time?” ",
    "said ",
    "aloud. ",
    "must ",
    "be ",
    "getting ",
    "somewhere ",
    "near ",
    "centre ",
    "earth. ",
    "Let ",
    "see: ",
    "would ",
    "four ",
    "thousand ",
    "think—” ",
    "(for, ",
    "you ",
    "see, ",
    "learnt ",
    "several ",
    "things ",
    "sort ",
    "lessons ",
    "schoolroom, ",
    "though ",
    "very",
    "good ",
    "opportunity ",
    "showing ",
    "knowledge, ",
    "no ",
    "listen ",
    "her, ",
    "still ",
    "practice ",
    "over) ",
    "“—yes, ",
    "that’s ",
    "right ",
    "distance—but ",
    "Latitude ",
    "Longitude ",
    "got ",
    "to?” ",
    "(Alice ",
    "idea ",
    "was, ",
    "either, ",
    "nice ",
    "grand ",
    "words ",
    "say.)",
    "Presently ",
    "began ",
    "again. ",
    "through",
    "earth! ",
    "funny ",
    "it’ll ",
    "seem ",
    "among ",
    "people ",
    "walk ",
    "their ",
    "heads ",
    "downward! ",
    "Antipathies, ",
    "(she ",
    "rather ",
    "glad ",
    "was",
    "listening, ",
    "time, ",
    "didn’t ",
    "sound ",
    "word) ",
    "“—but ",
    "ask ",
    "them ",
    "name ",
    "country ",
    "is, ",
    "know. ",
    "Please, ",
    "Ma’am, ",
    "is ",
    "New ",
    "Zealand ",
    "Australia?” ",
    "(and ",
    "curtsey ",
    "spoke—fancy ",
    "curtseying",
    "you’re ",
    "through ",
    "air! ",
    "Do ",
    "could ",
    "manage ",
    "it?) ",
    "“And ",
    "ignorant ",
    "little ",
    "girl ",
    "she’ll ",
    "asking! ",
    "No, ",
    "do ",
    "ask: ",
    "perhaps ",
    "written ",
    "up ",
    "somewhere.”",
    "There ",
    "else ",
    "do, ",
    "soon ",
    "talking ",
    "“Dinah’ll ",
    "miss ",
    "much ",
    "to-night, ",
    "should ",
    "think!” ",
    "(Dinah ",
    "cat.) ",
    "hope ",
    "remember ",
    "saucer ",
    "milk ",
    "tea-",
    "time. ",
    "Dinah ",
    "my ",
    "dear! ",
    "wish ",
    "me! ",
    "are ",
    "mice ",
    "air, ",
    "I’m ",
    "afraid, ",
    "might ",
    "catch ",
    "bat, ",
    "mouse, ",
    "But ",
    "cats ",
    "eat ",
    "bats, ",
    "wonder?” ",
    "And ",
    "sleepy, ",
    "saying ",
    "dreamy ",
    "“Do ",
    "bats? ",
    "bats?” ",
    "sometimes, ",
    "bats ",
    "cats?” ",
    "for, ",
    "couldn’t ",
    "answer ",
    "question, ",
    "matter ",
    "which ",
    "way ",
    "it. ",
    "felt ",
    "dozing ",
    "off, ",
    "begun ",
    "dream ",
    "walking ",
    "hand ",
    "Dinah, ",
    "earnestly, ",
    "“Now, ",
    "tell ",
};

constexpr Word words0[] = {
    {0, 48, false}, {1, 142, false}, {2, 338, false}, {3, 390, false}, {4, 505, false},
    {2, 777, false}, {5, 829, false},
};
constexpr Word words1[] = {
    {6, 48, false}, {7, 112, false}, {8, 204, false}, {0, 316, false}, {9, 409, false},
    {6, 556, false}, {10, 619, false}, {11, 740, false}, {12, 979, false},
};
constexpr Word words2[] = {
    {13, 48, false}, {14, 172, false}, {12, 275, false}, {15, 348, false}, {16, 448, false},
    {2, 576, false}, {17, 637, false}, {18, 718, false}, {19, 911, false},
};
constexpr Word words3[] = {
    {20, 48, false}, {14, 256, false}, {21, 346, false}, {15, 483, false}, {22, 571, false},
    {23, 737, false}, {24, 930, true},
};
constexpr Word words4[] = {
    {25, 48, true}, {26, 108, true}, {27, 271, true}, {28, 374, true}, {29, 446, true},
    {30, 531, true}, {31, 897, false}, {32, 939, false},
};
constexpr Word words5[] = {
    {33, 48, false}, {12, 227, false}, {34, 302, false}, {32, 377, false}, {35, 491, false},
    {36, 622, false}, {37, 818, false}, {38, 913, false},
};
constexpr Word words6[] = {
    {39, 48, false}, {6, 224, false}, {7, 286, false}, {40, 376, false}, {41, 488, false},
    {2, 569, false}, {42, 619, false}, {43, 795, false}, {7, 951, false},
};
constexpr Word words7[] = {
    {44, 48, false}, {8, 196, false}, {0, 320, false}, {45, 426, false}, {46, 541, false},
    {47, 697, false}, {48, 868, false}, {25, 998, false},
};
constexpr Word words8[] = {
    {49, 48, false}, {50, 203, false}, {51, 325, false}, {25, 476, false}, {52, 521, false},
    {53, 931, false}, {25, 998, false},
};
constexpr Word words9[] = {
    {26, 48, false}, {6, 204, false}, {54, 271, false}, {27, 389, false}, {28, 484, false},
    {34, 552, false}, {32, 618, false}, {55, 724, false}, {50, 922, false},
};
constexpr Word words10[] = {
    {56, 48, false}, {0, 277, false}, {57, 375, false}, {43, 474, false}, {15, 637, false},
    {58, 732, false}, {59, 850, false}, {34, 979, false},
};
constexpr Word words11[] = {
    {32, 48, false}, {60, 156, false}, {61, 429, false}, {62, 537, false}, {63, 641, false},
    {16, 708, false}, {6, 831, false}, {64, 898, false}, {2, 990, false},
};
constexpr Word words12[] = {
    {65, 48, false}, {66, 164, false}, {25, 319, false}, {67, 375, false}, {68, 516, false},
    {69, 791, false}, {15, 952, false},
};
constexpr Word words13[] = {
    {70, 48, false},
};
constexpr Word words14[] = {
    {71, 102, false}, {72, 184, false}, {73, 393, false}, {66, 613, false}, {74, 770, false},
    {38, 913, false},
};
constexpr Word words15[] = {
    {59, 48, false}, {34, 173, false}, {46, 236, false}, {75, 379, false}, {76, 501, false},
    {77, 780, false}, {63, 890, false}, {15, 952, false},
};
constexpr Word words16[] = {
    {78, 48, false}, {0, 188, false}, {61, 273, false}, {6, 370, false}, {79, 426, false},
    {27, 506, false}, {80, 591, false},
};
constexpr Word words17[] = {
    {81, 102, false}, {68, 209, false}, {74, 477, false}, {82, 612, false}, {83, 812, false},
    {84, 893, false}, {25, 998, false},
};
constexpr Word words18[] = {
    {85, 48, false}, {41, 229, false}, {86, 329, false}, {87, 479, false}, {32, 612, false},
    {35, 732, false}, {88, 869, false},
};
constexpr Word words19[] = {
    {89, 48, false}, {90, 268, false}, {91, 420, false}, {89, 486, false}, {8, 706, false},
    {38, 813, false}, {45, 940, false},
};
constexpr Word words20[] = {
    {92, 48, false}, {25, 157, false}, {73, 216, false}, {6, 435, false}, {93, 513, false},
    {94, 668, false}, {95, 828, false},
};
constexpr Word words21[] = {
    {96, 48, false}, {47, 238, false}, {0, 414, false}, {97, 524, false}, {96, 691, false},
    {98, 880, false},
};
constexpr Word words22[] = {
    {66, 48, false}, {25, 183, false}, {99, 219, false}, {100, 329, false}, {101, 445, false},
};
constexpr Word words23[] = {
    {102, 102, false}, {15, 256, false}, {103, 344, false}, {61, 450, false}, {99, 552, false},
    {104, 667, false}, {53, 800, false}, {0, 863, false}, {105, 954, false},
};
constexpr Word words24[] = {
    {99, 48, false}, {106, 168, false}, {41, 339, false}, {0, 425, false}, {45, 521, false},
    {107, 625, false}, {28, 788, false}, {16, 855, false}, {108, 976, false},
};
constexpr Word words25[] = {
    {0, 48, false}, {74, 147, false}, {66, 283, false}, {6, 431, false}, {109, 500, false},
    {94, 621, false}, {7, 773, false}, {32, 870, false}, {6, 979, false},
};
constexpr Word words26[] = {
    {110, 48, false}, {111, 243, false}, {61, 382, false}, {112, 496, false}, {6, 647, false},
    {113, 719, false}, {114, 913, false},
};
constexpr Word words27[] = {
    {115, 48, false}, {0, 180, false}, {116, 272, false}, {6, 397, false}, {109, 459, false},
    {66, 572, false}, {32, 712, false}, {117, 813, false}, {27, 950, false},
};
constexpr Word words28[] = {
    {111, 48, false}, {0, 173, false}, {61, 260, false}, {118, 359, false}, {119, 536, false},
    {14, 605, false}, {2, 692, false}, {61, 737, false}, {120, 835, false}, {121, 919, false},
};
constexpr Word words29[] = {
    {6, 48, false}, {64, 105, false}, {122, 187, false}, {35, 414, false}, {0, 527, false},
    {33, 613, false}, {12, 773, false}, {15, 829, false}, {123, 912, false},
};
constexpr Word words30[] = {
    {28, 48, false}, {15, 108, false}, {124, 192, false}, {32, 306, false}, {125, 403, false},
    {8, 582, false}, {126, 687, false}, {127, 796, false}, {128, 914, false},
};
constexpr Word words31[] = {
    {50, 48, false}, {129, 165, false}, {32, 412, false}, {130, 512, false}, {131, 828, false},
    {32, 939, false},
};
constexpr Word words32[] = {
    {132, 48, false}, {0, 177, false}, {133, 265, false}, {134, 363, false}, {32, 496, false},
    {135, 593, false}, {136, 785, false}, {137, 910, false},
};
constexpr Word words33[] = {
    {138, 48, false}, {139, 179, false}, {24, 279, false}, {66, 398, false}, {25, 540, false},
    {140, 584, false}, {141, 661, false}, {142, 788, false}, {28, 887, false}, {15, 952, false},
};
constexpr Word words34[] = {
    {143, 48, false}, {108, 249, false}, {0, 336, false}, {144, 450, false}, {2, 654, false},
    {61, 726, false}, {145, 851, false},
};
constexpr Word words35[] = {
    {146, 48, false}, {147, 293, false}, {14, 662, false}, {6, 755, false}, {7, 818, false},
    {148, 909, false},
};
constexpr Word words36[] = {
    {149, 48, false}, {2, 425, false}, {61, 478, false}, {150, 584, false}, {0, 760, false},
    {151, 855, false}, {92, 948, false},
};
constexpr Word words37[] = {
    {84, 48, false}, {6, 163, false}, {152, 242, false}, {15, 381, false}, {140, 487, false},
    {41, 580, false}, {153, 679, false}, {28, 801, false}, {154, 881, false},
};
constexpr Word words38[] = {
    {155, 48, false}, {156, 286, false}, {91, 569, false}, {157, 630, false}, {6, 846, false},
    {158, 903, false}, {2, 990, false},
};
constexpr Word words39[] = {
    {159, 48, false}, {142, 149, false}, {28, 241, false}, {15, 299, false}, {129, 382, false},
    {108, 625, false}, {0, 684, false}, {105, 770, false}, {160, 851, false}, {161, 958, false},
};
constexpr Word words40[] = {
    {162, 102, false}, {1, 285, false}, {38, 491, false}, {6, 632, false}, {163, 707, false},
    {164, 898, false},
};
constexpr Word words41[] = {
    {165, 48, false}, {25, 169, false}, {166, 213, false}, {108, 304, false}, {13, 370, false},
    {167, 484, false}, {168, 520, false}, {93, 643, false}, {169, 783, false}, {28, 977, false},
};
constexpr Word words42[] = {
    {81, 48, false}, {68, 164, false}, {74, 441, false}, {82, 585, false}, {83, 794, false},
    {84, 884, false}, {25, 998, false},
};
constexpr Word words43[] = {
    {162, 75, false}, {1, 240, false}, {38, 428, false}, {6, 552, false}, {163, 610, false},
    {164, 784, false}, {165, 922, false},
};
constexpr Word words44[] = {
    {25, 48, false}, {166, 108, false}, {108, 214, false}, {13, 295, false}, {167, 424, false},
    {168, 475, false}, {93, 613, false}, {169, 768, false}, {28, 977, false},
};
constexpr Word words45[] = {
    {170, 48, false}, {66, 267, false}, {171, 407, false}, {172, 562, false}, {173, 678, false},
    {174, 816, false}, {17, 970, false},
};
constexpr Word words46[] = {
    {93, 48, false}, {175, 200, false}, {12, 297, false}, {176, 372, false}, {177, 538, false},
    {167, 680, false}, {178, 727, false}, {179, 951, false},
};
constexpr Word words47[] = {
    {180, 48, false}, {94, 262, false}, {34, 402, false}, {181, 459, false}, {182, 576, false},
    {167, 623, false}, {105, 653, false}, {183, 735, false}, {15, 809, false}, {184, 892, false},
    {28, 977, false},
};
constexpr Word words48[] = {
    {15, 48, false}, {185, 131, false}, {186, 305, false}, {61, 474, false}, {99, 570, false},
    {187, 680, false}, {188, 813, false},
};
constexpr Word words49[] = {
    {189, 102, false}, {90, 261, false}, {190, 413, false}, {191, 564, false}, {15, 725, false},
    {166, 814, false}, {192, 904, true}, {193, 1024, false},
};
constexpr Word words50[] = {
    {194, 48, false}, {6, 179, false}, {195, 238, false}, {196, 308, false}, {197, 429, false},
    {110, 481, false}, {77, 662, false}, {198, 768, false}, {199, 906, false},
};
constexpr Word words51[] = {
    {200, 48, false}, {201, 148, false}, {202, 293, false}, {203, 365, false}, {204, 468, false},
    {0, 630, false}, {205, 723, false}, {206, 833, false}, {197, 985, false},
};
constexpr Word words52[] = {
    {207, 48, false}, {208, 175, false}, {209, 242, false}, {210, 416, false}, {211, 682, false},
    {15, 797, false}, {212, 884, false},
};
constexpr Word words53[] = {
    {28, 48, false}, {15, 110, false}, {213, 197, false}, {214, 342, false}, {175, 428, false},
    {215, 510, false}, {8, 608, false}, {216, 715, false}, {208, 865, false}, {217, 931, false},
};
constexpr Word words54[] = {
    {218, 48, false}, {199, 283, false}, {90, 426, false}, {167, 585, false}, {219, 627, false},
    {220, 829, false}, {221, 942, false},
};
constexpr Word words55[] = {
    {222, 48, false}, {38, 147, false}, {45, 278, false}, {223, 382, false}, {224, 536, false},
    {225, 713, false}, {28, 873, false}, {203, 939, false},
};
constexpr Word words56[] = {
    {226, 48, false}, {63, 157, false}, {7, 222, false}, {227, 315, false}, {63, 495, false},
    {15, 559, false}, {228, 650, false}, {32, 939, false},
};
constexpr Word words57[] = {
    {229, 48, false}, {203, 264, false}, {61, 405, false}, {92, 548, false}, {25, 681, false},
    {230, 764, true}, {193, 858, false}, {231, 915, false},
};
constexpr Word words58[] = {
    {232, 48, false}, {41, 333, false}, {233, 412, false}, {183, 612, false}, {7, 687, false},
    {234, 773, false},
};
constexpr Word words59[] = {
    {108, 48, false}, {132, 114, false}, {61, 247, false}, {235, 351, false}, {142, 426, false},
    {6, 525, false}, {236, 587, false}, {6, 727, false}, {237, 789, false}, {238, 889, false},
    {2, 990, false},
};
constexpr Word words60[] = {
    {61, 48, false}, {231, 163, false}, {239, 301, false}, {6, 508, false}, {179, 581, false},
    {2, 682, false}, {240, 744, false}, {241, 883, false},
};
constexpr Word words61[] = {
    {242, 48, false}, {94, 193, false}, {15, 340, false}, {243, 431, false}, {244, 560, false},
    {35, 885, false}, {167, 1006, false},
};
constexpr Word words62[] = {
    {110, 48, false}, {111, 229, false}, {245, 355, false}, {53, 557, false}, {246, 618, false},
    {200, 858, false}, {247, 952, false},
};
constexpr Word words63[] = {
    {248, 48, false}, {249, 154, false}, {45, 299, false}, {235, 402, false}, {250, 478, false},
    {111, 590, false}, {245, 721, false}, {251, 927, false},
};
constexpr Word words64[] = {
    {53, 48, false}, {246, 113, false}, {252, 357, false}, {14, 516, false}, {1, 609, false},
    {126, 804, false}, {127, 918, false},
};
constexpr Word words65[] = {
    {253, 48, false}, {254, 151, false}, {255, 292, false}, {6, 439, false}, {256, 495, false},
};
constexpr Word words66[] = {
    {257, 102, false}, {0, 329, false}, {258, 420, false}, {259, 569, false}, {197, 717, false},
    {110, 771, false}, {182, 955, false}, {167, 1006, false},
};
constexpr Word words67[] = {
    {168, 48, false}, {166, 188, false}, {243, 296, false}, {260, 441, true}, {193, 614, false},
    {15, 649, false}, {261, 756, false}, {172, 923, false},
};
constexpr Word words68[] = {
    {262, 48, false}, {263, 209, false}, {264, 311, false}, {6, 452, false}, {194, 525, false},
    {27, 670, false}, {265, 772, false}, {15, 952, false},
};
constexpr Word words69[] = {
    {266, 48, false}, {8, 254, false}, {267, 405, false}, {50, 569, false}, {268, 729, false},
    {269, 894, false},
};
constexpr Word words70[] = {
    {270, 48, false}, {81, 315, false}, {271, 413, false}, {167, 704, false}, {219, 739, false},
    {272, 934, false},
};
constexpr Word words71[] = {
    {61, 48, false}, {273, 151, false}, {274, 306, false}, {132, 417, false}, {275, 549, true},
    {193, 633, false}, {235, 649, false}, {142, 722, false}, {276, 819, false},
};
constexpr Word words72[] = {
    {203, 48, false}, {277, 150, false}, {108, 279, false}, {2, 344, false}, {278, 395, false},
    {279, 543, false}, {12, 695, false}, {17, 756, false}, {15, 826, false}, {243, 914, false},
};
constexpr Word words73[] = {
    {280, 48, false}, {281, 194, false}, {167, 346, false}, {168, 382, false}, {10, 504, false},
    {6, 625, false}, {282, 687, false}, {283, 781, false}, {111, 912, false},
};
constexpr Word words74[] = {
    {15, 48, false}, {284, 153, false}, {28, 309, false}, {15, 388, false}, {285, 492, false},
    {286, 699, false}, {221, 778, false}, {287, 892, false},
};
constexpr Word words75[] = {
    {288, 48, false}, {289, 236, false}, {290, 434, false}, {203, 507, false}, {291, 628, false},
    {292, 762, false}, {53, 977, false},
};
constexpr Word words76[] = {
    {293, 48, false}, {294, 311, false}, {0, 427, false}, {116, 519, false}, {6, 644, false},
    {295, 706, false}, {108, 885, false}, {0, 949, false},
};
constexpr Word words77[] = {
    {296, 48, false}, {297, 376, true}, {193, 604, false}, {108, 635, false}, {298, 713, false},
    {98, 880, false},
};
constexpr Word words78[] = {
    {299, 48, false}, {15, 252, false}, {300, 350, false}, {301, 452, false}, {221, 540, false},
    {93, 648, false}, {221, 795, false}, {302, 903, false},
};
constexpr Word words79[] = {
    {303, 48, false}, {304, 249, false}, {305, 348, false}, {111, 483, false}, {195, 620, false},
    {306, 701, false}, {307, 919, false},
};
constexpr Word words80[] = {
    {308, 48, false}, {309, 137, false}, {93, 263, false}, {175, 397, false}, {41, 476, false},
    {310, 553, false}, {311, 727, false}, {263, 813, false}, {46, 899, false},
};
constexpr Word words81[] = {
    {312, 48, false}, {6, 122, false}, {313, 185, false}, {314, 293, false}, {167, 490, false},
    {168, 525, false}, {64, 647, false}, {2, 734, false}, {315, 784, false}, {316, 967, false},
};
constexpr Word words82[] = {
    {317, 48, false},
};
constexpr Word words83[] = {
    {189, 102, false}, {90, 269, false}, {190, 429, false}, {318, 588, false}, {61, 738, false},
    {169, 848, false},
};
constexpr Word words84[] = {
    {319, 48, false}, {6, 144, false}, {320, 201, false}, {91, 280, false}, {38, 338, false},
    {321, 460, false}, {258, 577, false}, {322, 721, false}, {259, 891, false},
};
constexpr Word words85[] = {
    {323, 48, false}, {324, 275, false}, {175, 407, false}, {99, 503, false}, {325, 632, false},
    {326, 785, false}, {167, 1006, false},
};
constexpr Word words86[] = {
    {327, 48, false}, {328, 215, false}, {329, 388, false}, {61, 556, false}, {15, 660, false},
    {330, 749, false}, {197, 859, false}, {331, 915, false},
};
constexpr Word words87[] = {
    {174, 48, false}, {332, 204, false}, {7, 447, false}, {333, 537, false}, {28, 697, false},
    {334, 761, false}, {12, 879, false}, {335, 941, false},
};
constexpr Word words88[] = {
    {336, 48, false}, {337, 175, false}, {338, 325, false}, {339, 408, false}, {167, 536, false},
    {340, 567, false}, {221, 685, false}, {127, 780, false}, {66, 900, false},
};
constexpr Word words89[] = {
    {131, 48, false}, {50, 161, false}, {341, 280, false}, {318, 377, false}, {342, 521, false},
    {235, 608, false}, {343, 682, false}, {63, 804, false}, {15, 865, false}, {344, 953, false},
};
constexpr Word words90[] = {
    {345, 48, false}, {346, 140, false}, {14, 306, false}, {221, 400, false}, {347, 502, false},
    {348, 653, false}, {25, 791, false}, {349, 836, false}, {32, 939, false},
};
constexpr Word words91[] = {
    {242, 48, false}, {99, 190, false}, {84, 307, false}, {25, 406, false}, {350, 450, false},
    {221, 622, false}, {287, 722, false}, {351, 872, false}, {312, 968, false},
};
constexpr Word words92[] = {
    {352, 48, false}, {353, 165, false}, {354, 260, false}, {167, 392, false}, {355, 435, false},
    {356, 672, false}, {131, 792, false}, {38, 913, false},
};
constexpr Word words93[] = {
    {258, 48, false}, {6, 209, false}, {79, 282, false}, {273, 378, false}, {357, 543, false},
    {32, 718, false}, {74, 829, false}, {83, 967, false},
};
constexpr Word words94[] = {
    {358, 48, false}, {6, 212, false}, {163, 276, false}, {63, 457, false}, {25, 521, false},
    {359, 566, false}, {226, 753, false}, {28, 861, false}, {87, 926, false},
};
constexpr Word words95[] = {
    {360, 48, false}, {352, 154, false}, {353, 265, false}, {361, 355, false}, {301, 495, false},
    {352, 579, false}, {353, 690, false}, {362, 780, false}, {32, 939, false},
};
constexpr Word words96[] = {
    {363, 48, false}, {360, 313, false}, {364, 413, false}, {353, 524, false}, {365, 609, false},
    {366, 757, false}, {221, 846, false}, {222, 943, false},
};
constexpr Word words97[] = {
    {108, 48, false}, {0, 120, false}, {367, 219, false}, {368, 423, false}, {51, 607, false},
    {369, 762, false}, {2, 990, false},
};
constexpr Word words98[] = {
    {278, 48, false}, {325, 202, false}, {370, 347, false}, {371, 518, false}, {372, 673, false},
    {0, 784, false}, {158, 881, false}, {373, 979, false},
};
constexpr Word words99[] = {
    {139, 48, false}, {374, 152, false}, {8, 250, false}, {0, 365, false}, {61, 461, false},
    {375, 569, false}, {376, 741, false}, {32, 833, false}, {45, 940, false},
};
constexpr Word words100[] = {
    {62, 48, false}, {377, 151, false}, {6, 307, false}, {378, 372, false}, {8, 534, false},
    {0, 647, false}, {61, 741, false}, {379, 846, false},
};
constexpr Word words101[] = {
    {380, 48, false}, {63, 176, false}, {380, 235, false}, {50, 362, false}, {381, 477, false},
    {32, 637, false}, {358, 735, false}, {6, 893, false}, {7, 951, false},
};
constexpr Word words102[] = {
    {99, 48, false}, {382, 177, false}, {383, 422, false}, {381, 581, false}, {384, 757, false},
    {175, 856, false}, {15, 952, false},
};

constexpr Line lines0[] = {
    {words0, 7, 81}, {words1, 9, 145}, {words2, 9, 209},
    {words3, 7, 273}, {words4, 8, 337}, {words5, 8, 401},
    {words6, 9, 465}, {words7, 8, 529}, {words8, 7, 593},
    {words9, 9, 657}, {words10, 8, 721}, {words11, 9, 785},
    {words12, 7, 849}, {words13, 1, 913}, {words14, 6, 977},
    {words15, 8, 1041}, {words16, 7, 1105}, {words17, 7, 1169},
    {words18, 7, 1233}, {words19, 7, 1297}, {words20, 7, 1361},
};

constexpr Line lines1[] = {
    {words21, 6, 81}, {words22, 5, 145}, {words23, 9, 209},
    {words24, 9, 273}, {words25, 9, 337}, {words26, 7, 401},
    {words27, 9, 465}, {words28, 10, 529}, {words29, 9, 593},
    {words30, 9, 657}, {words31, 6, 721}, {words32, 8, 785},
    {words33, 10, 849}, {words34, 7, 913}, {words35, 6, 977},
    {words36, 7, 1041}, {words37, 9, 1105}, {words38, 7, 1169},
    {words39, 10, 1233}, {words40, 6, 1297}, {words41, 10, 1361},
};

constexpr Line lines2[] = {
    {words0, 7, 97}, {words1, 9, 161}, {words2, 9, 225},
    {words3, 7, 289}, {words4, 8, 353}, {words5, 8, 417},
    {words6, 9, 481}, {words7, 8, 545}, {words8, 7, 609},
    {words9, 9, 673}, {words10, 8, 737}, {words11, 9, 801},
    {words12, 7, 865}, {words13, 1, 929}, {words14, 6, 993},
    {words15, 8, 1057}, {words16, 7, 1121}, {words17, 7, 1185},
    {words18, 7, 1249}, {words19, 7, 1313}, {words20, 7, 1377},
};

constexpr Line lines3[] = {
    {words21, 6, 97}, {words22, 5, 161}, {words23, 9, 225},
    {words24, 9, 289}, {words25, 9, 353}, {words26, 7, 417},
    {words27, 9, 481}, {words28, 10, 545}, {words29, 9, 609},
    {words30, 9, 673}, {words31, 6, 737}, {words32, 8, 801},
    {words33, 10, 865}, {words34, 7, 929}, {words35, 6, 993},
    {words36, 7, 1057}, {words37, 9, 1121}, {words38, 7, 1185},
    {words39, 10, 1249}, {words40, 6, 1313}, {words41, 10, 1377},
};

constexpr Line lines4[] = {
    {words0, 7, 81}, {words1, 9, 146}, {words2, 9, 212},
    {words3, 7, 277}, {words4, 8, 343}, {words5, 8, 409},
    {words6, 9, 474}, {words7, 8, 540}, {words8, 7, 605},
    {words9, 9, 671}, {words10, 8, 737}, {words11, 9, 802},
    {words12, 7, 868}, {words13, 1, 933}, {words14, 6, 999},
    {words15, 8, 1065}, {words16, 7, 1130}, {words17, 7, 1196},
    {words18, 7, 1261}, {words19, 7, 1327}, {words20, 7, 1393},
};

constexpr Line lines5[] = {
    {words21, 6, 81}, {words22, 5, 146}, {words23, 9, 212},
    {words24, 9, 277}, {words25, 9, 343}, {words26, 7, 409},
    {words27, 9, 474}, {words28, 10, 540}, {words29, 9, 605},
    {words30, 9, 671}, {words31, 6, 737}, {words32, 8, 802},
    {words33, 10, 868}, {words34, 7, 933}, {words35, 6, 999},
    {words36, 7, 1065}, {words37, 9, 1130}, {words38, 7, 1196},
    {words39, 10, 1261}, {words40, 6, 1327}, {words41, 10, 1393},
};

constexpr Line lines6[] = {
    {words42, 7, 81}, {words18, 7, 145}, {words19, 7, 209},
    {words20, 7, 273}, {words21, 6, 337}, {words22, 5, 401},
    {words23, 9, 476}, {words24, 9, 540}, {words25, 9, 604},
    {words26, 7, 668}, {words27, 9, 732}, {words28, 10, 796},
    {words29, 9, 860}, {words30, 9, 924}, {words31, 6, 988},
    {words32, 8, 1052}, {words33, 10, 1116}, {words34, 7, 1180},
    {words35, 6, 1244}, {words36, 7, 1308}, {words37, 9, 1372},
};

constexpr Line lines7[] = {
    {words38, 7, 81}, {words39, 10, 145}, {words43, 7, 278},
    {words44, 9, 342}, {words45, 7, 406}, {words46, 8, 470},
    {words47, 11, 534}, {words48, 7, 598}, {words49, 8, 662},
    {words50, 9, 726}, {words51, 9, 790}, {words52, 7, 854},
    {words53, 10, 918}, {words54, 7, 982}, {words55, 8, 1046},
    {words56, 8, 1110}, {words57, 8, 1174}, {words58, 6, 1238},
    {words59, 11, 1302}, {words60, 8, 1366},
};

constexpr Line lines8[] = {
    {words61, 7, 81}, {words62, 7, 145}, {words63, 8, 209},
    {words64, 7, 273}, {words65, 5, 337}, {words66, 8, 401},
    {words67, 8, 465}, {words68, 8, 529}, {words69, 6, 593},
    {words70, 6, 657}, {words71, 9, 721}, {words72, 10, 785},
    {words73, 9, 849}, {words74, 8, 913}, {words75, 7, 977},
    {words76, 8, 1041}, {words77, 6, 1105}, {words78, 8, 1169},
    {words79, 7, 1233}, {words80, 9, 1297},
};

constexpr Line lines9[] = {
    {words81, 10, 81}, {words82, 1, 145}, {words83, 6, 209},
    {words84, 9, 273}, {words85, 7, 337}, {words86, 8, 401},
    {words87, 8, 465}, {words88, 9, 529}, {words89, 10, 593},
    {words90, 9, 657}, {words91, 9, 721}, {words92, 8, 785},
    {words93, 8, 849}, {words94, 9, 913}, {words95, 9, 977},
    {words96, 8, 1041}, {words97, 7, 1105}, {words98, 8, 1169},
    {words99, 9, 1233}, {words100, 8, 1297}, {words101, 9, 1361},
};

constexpr Line lines10[] = {
    {words61, 7, 129}, {words62, 7, 193}, {words63, 8, 257},
    {words64, 7, 321}, {words65, 5, 385}, {words66, 8, 449},
    {words67, 8, 513}, {words68, 8, 577}, {words69, 6, 641},
    {words70, 6, 705}, {words71, 9, 769}, {words72, 10, 833},
    {words73, 9, 897}, {words74, 8, 961}, {words75, 7, 1025},
    {words76, 8, 1089}, {words77, 6, 1153}, {words78, 8, 1217},
    {words79, 7, 1281}, {words80, 9, 1345},
};

constexpr Line lines11[] = {
    {words61, 7, 81}, {words62, 7, 150}, {words63, 8, 219},
    {words64, 7, 288}, {words65, 5, 357}, {words66, 8, 426},
    {words67, 8, 495}, {words68, 8, 564}, {words69, 6, 633},
    {words70, 6, 702}, {words71, 9, 771}, {words72, 10, 840},
    {words73, 9, 909}, {words74, 8, 978}, {words75, 7, 1047},
    {words76, 8, 1116}, {words77, 6, 1185}, {words78, 8, 1254},
    {words79, 7, 1323}, {words80, 9, 1393},
};

constexpr Line lines12[] = {
    {words61, 7, 97}, {words62, 7, 161}, {words63, 8, 225},
    {words64, 7, 289}, {words65, 5, 353}, {words66, 8, 417},
    {words67, 8, 481}, {words68, 8, 545}, {words69, 6, 609},
    {words70, 6, 673}, {words71, 9, 737}, {words72, 10, 801},
    {words73, 9, 865}, {words74, 8, 929}, {words75, 7, 993},
    {words76, 8, 1057}, {words77, 6, 1121}, {words78, 8, 1185},
    {words79, 7, 1249}, {words80, 9, 1313}, {words81, 10, 1377},
};

constexpr Line lines13[] = {
    {words82, 1, 97}, {words83, 6, 161}, {words84, 9, 225},
    {words85, 7, 289}, {words86, 8, 353}, {words87, 8, 417},
    {words88, 9, 481}, {words89, 10, 545}, {words90, 9, 609},
    {words91, 9, 673}, {words92, 8, 737}, {words93, 8, 801},
    {words94, 9, 865}, {words95, 9, 929}, {words96, 8, 993},
    {words97, 7, 1057}, {words98, 8, 1121}, {words99, 9, 1185},
    {words100, 8, 1249}, {words101, 9, 1313}, {words102, 7, 1377},
};

constexpr Line lines14[] = {
    {words81, 10, 97}, {words82, 1, 161}, {words83, 6, 225},
    {words84, 9, 289}, {words85, 7, 353}, {words86, 8, 417},
    {words87, 8, 481}, {words88, 9, 545}, {words89, 10, 609},
    {words90, 9, 673}, {words91, 9, 737}, {words92, 8, 801},
    {words93, 8, 865}, {words94, 9, 929}, {words95, 9, 993},
    {words96, 8, 1057}, {words97, 7, 1121}, {words98, 8, 1185},
    {words99, 9, 1249}, {words100, 8, 1313}, {words101, 9, 1377},
};

constexpr Line lines15[] = {
    {words61, 7, 81}, {words62, 7, 145}, {words63, 8, 209},
    {words64, 7, 273}, {words65, 5, 337}, {words66, 8, 401},
    {words67, 8, 465}, {words68, 8, 529}, {words69, 6, 593},
    {words70, 6, 657}, {words71, 9, 721}, {words72, 10, 785},
    {words73, 9, 849}, {words74, 8, 913}, {words75, 7, 977},
    {words76, 8, 1041}, {words77, 6, 1105}, {words78, 8, 1169},
    {words79, 7, 1233}, {words80, 9, 1297}, {words81, 10, 1361},
};

constexpr Line lines16[] = {
    {words82, 1, 81}, {words83, 6, 145}, {words84, 9, 209},
    {words85, 7, 273}, {words86, 8, 337}, {words87, 8, 401},
    {words88, 9, 465}, {words89, 10, 529}, {words90, 9, 593},
    {words91, 9, 657}, {words92, 8, 721}, {words93, 8, 785},
    {words94, 9, 849}, {words95, 9, 913}, {words96, 8, 977},
    {words97, 7, 1041}, {words98, 8, 1105}, {words99, 9, 1169},
    {words100, 8, 1233}, {words101, 9, 1297}, {words102, 7, 1361},
};

constexpr Line lines17[] = {
    {words61, 7, 81}, {words62, 7, 146}, {words63, 8, 212},
    {words64, 7, 277}, {words65, 5, 343}, {words66, 8, 409},
    {words67, 8, 474}, {words68, 8, 540}, {words69, 6, 605},
    {words70, 6, 671}, {words71, 9, 737}, {words72, 10, 802},
    {words73, 9, 868}, {words74, 8, 933}, {words75, 7, 999},
    {words76, 8, 1065}, {words77, 6, 1130}, {words78, 8, 1196},
    {words79, 7, 1261}, {words80, 9, 1327}, {words81, 10, 1393},
};

constexpr Line lines18[] = {
    {words82, 1, 81}, {words83, 6, 146}, {words84, 9, 212},
    {words85, 7, 277}, {words86, 8, 343}, {words87, 8, 409},
    {words88, 9, 474}, {words89, 10, 540}, {words90, 9, 605},
    {words91, 9, 671}, {words92, 8, 737}, {words93, 8, 802},
    {words94, 9, 868}, {words95, 9, 933}, {words96, 8, 999},
    {words97, 7, 1065}, {words98, 8, 1130}, {words99, 9, 1196},
    {words100, 8, 1261}, {words101, 9, 1327}, {words102, 7, 1393},
};

constexpr Line lines19[] = {
    {words81, 10, 81}, {words82, 1, 146}, {words83, 6, 212},
    {words84, 9, 277}, {words85, 7, 343}, {words86, 8, 409},
    {words87, 8, 474}, {words88, 9, 540}, {words89, 10, 605},
    {words90, 9, 671}, {words91, 9, 737}, {words92, 8, 802},
    {words93, 8, 868}, {words94, 9, 933}, {words95, 9, 999},
    {words96, 8, 1065}, {words97, 7, 1130}, {words98, 8, 1196},
    {words99, 9, 1261}, {words100, 8, 1327}, {words101, 9, 1393},
};

constexpr Page pages[] = {
    {lines0, 21},
    {lines1, 21},
    {lines2, 21},
    {lines3, 21},
    {lines4, 21},
    {lines5, 21},
    {lines6, 21},
    {lines7, 20},
    {lines8, 20},
    {lines9, 21},
    {lines10, 20},
    {lines11, 20},
    {lines12, 21},
    {lines13, 21},
    {lines14, 21},
    {lines15, 21},
    {lines16, 21},
    {lines17, 21},
    {lines18, 21},
    {lines19, 21},
};

constexpr Band bands0[] = {
    {44, 92}, {108, 156}, {172, 220}, {236, 283}, {300, 348},
    {364, 408}, {428, 472}, {492, 529}, {556, 604}, {620, 668},
    {684, 731}, {748, 795}, {812, 860}, {876, 924}, {940, 977},
    {1004, 1052}, {1068, 1116}, {1132, 1180}, {1196, 1244}, {1260, 1307},
    {1324, 1372},
};

constexpr Band bands1[] = {
    {60, 108}, {124, 172}, {188, 236}, {252, 299}, {316, 364},
    {380, 424}, {444, 488}, {508, 545}, {572, 620}, {636, 684},
    {700, 747}, {764, 811}, {828, 876}, {892, 940}, {956, 993},
    {1020, 1068}, {1084, 1132}, {1148, 1196}, {1212, 1260}, {1276, 1323},
    {1340, 1388},
};

constexpr Band bands2[] = {
    {44, 92}, {109, 157}, {175, 223}, {240, 287}, {306, 354},
    {372, 416}, {437, 481}, {503, 540}, {568, 616}, {634, 682},
    {700, 747}, {765, 812}, {831, 879}, {896, 944}, {962, 999},
    {1028, 1076}, {1093, 1141}, {1159, 1207}, {1224, 1272}, {1290, 1337},
    {1356, 1404},
};

constexpr Band bands3[] = {
    {44, 92}, {108, 156}, {172, 219}, {236, 284}, {300, 348},
    {364, 412}, {439, 487}, {503, 551}, {567, 604}, {631, 679},
    {695, 739}, {759, 807}, {823, 871}, {887, 934}, {951, 999},
    {1015, 1063}, {1079, 1127}, {1143, 1191}, {1207, 1255}, {1271, 1319},
    {1335, 1383},
};

constexpr Band bands4[] = {
    {44, 92}, {108, 156}, {172, 216}, {236, 284}, {300, 348},
    {364, 412}, {428, 476}, {492, 540}, {556, 604}, {620, 668},
    {684, 732}, {748, 796}, {812, 854}, {876, 923}, {940, 984},
    {1004, 1051}, {1068, 1116}, {1132, 1180}, {1196, 1244}, {1260, 1308},
};

constexpr Band bands5[] = {
    {92, 140}, {156, 204}, {220, 264}, {284, 332}, {348, 396},
    {412, 460}, {476, 524}, {540, 588}, {604, 652}, {668, 716},
    {732, 780}, {796, 844}, {860, 902}, {924, 971}, {988, 1032},
    {1052, 1099}, {1116, 1164}, {1180, 1228}, {1244, 1292}, {1308, 1356},
};

constexpr Band bands6[] = {
    {44, 92}, {113, 161}, {182, 226}, {251, 299}, {320, 368},
    {389, 437}, {458, 506}, {527, 575}, {596, 644}, {665, 713},
    {734, 782}, {803, 851}, {872, 914}, {941, 988}, {1010, 1054},
    {1079, 1126}, {1148, 1196}, {1217, 1265}, {1286, 1334}, {1356, 1404},
};

constexpr Band bands7[] = {
    {60, 108}, {124, 172}, {188, 232}, {252, 300}, {316, 364},
    {380, 428}, {444, 492}, {508, 556}, {572, 620}, {636, 684},
    {700, 748}, {764, 812}, {828, 870}, {892, 939}, {956, 1000},
    {1020, 1067}, {1084, 1132}, {1148, 1196}, {1212, 1260}, {1276, 1324},
    {1340, 1388},
};

constexpr Band bands8[] = {
    {44, 92}, {108, 156}, {172, 216}, {236, 284}, {300, 348},
    {364, 412}, {428, 476}, {492, 540}, {556, 604}, {620, 668},
    {684, 732}, {748, 796}, {812, 854}, {876, 923}, {940, 984},
    {1004, 1051}, {1068, 1116}, {1132, 1180}, {1196, 1244}, {1260, 1308},
    {1324, 1372},
};

constexpr Band bands9[] = {
    {44, 92}, {109, 157}, {175, 219}, {240, 288}, {306, 354},
    {372, 420}, {437, 485}, {503, 551}, {568, 616}, {634, 682},
    {700, 748}, {765, 813}, {831, 873}, {896, 943}, {962, 1006},
    {1028, 1075}, {1093, 1141}, {1159, 1207}, {1224, 1272}, {1290, 1338},
    {1356, 1404},
};

constexpr Option options0[] = {
    {0, 1, {bands0, 21}},
    {2, 3, {bands1, 21}},
    {4, 5, {bands2, 21}},
};

constexpr Option options1[] = {
    {6, 7, {bands3, 21}},
    {6, 7, {bands3, 21}},
    {6, 7, {bands3, 21}},
};

constexpr Option options2[] = {
    {0, 1, {bands0, 21}},
    {2, 3, {bands1, 21}},
    {4, 5, {bands2, 21}},
};

constexpr Option options3[] = {
    {8, 9, {bands4, 20}},
    {10, 9, {bands5, 20}},
    {11, 9, {bands6, 20}},
};

constexpr Option options4[] = {
    {0, 1, {bands0, 21}},
    {2, 3, {bands1, 21}},
    {4, 5, {bands2, 21}},
};

constexpr Option options5[] = {
    {6, 7, {bands3, 21}},
    {6, 7, {bands3, 21}},
    {6, 7, {bands3, 21}},
};

constexpr Option options6[] = {
    {2, 3, {bands1, 21}},
    {0, 1, {bands0, 21}},
};

constexpr Option options7[] = {
    {4, 5, {bands2, 21}},
    {0, 1, {bands0, 21}},
};

constexpr Option options8[] = {
    {6, 7, {bands3, 21}},
    {6, 7, {bands3, 21}},
};

constexpr Option options9[] = {
    {2, 3, {bands1, 21}},
    {0, 1, {bands0, 21}},
};

constexpr Option options10[] = {
    {4, 5, {bands2, 21}},
    {0, 1, {bands0, 21}},
};

constexpr Option options11[] = {
    {6, 7, {bands3, 21}},
    {6, 7, {bands3, 21}},
};

constexpr Option options12[] = {
    {2, 3, {bands1, 21}},
    {0, 1, {bands0, 21}},
};

constexpr Option options13[] = {
    {4, 5, {bands2, 21}},
    {0, 1, {bands0, 21}},
};

constexpr Option options14[] = {
    {6, 7, {bands3, 21}},
    {6, 7, {bands3, 21}},
};

constexpr Option options15[] = {
    {12, 13, {bands7, 21}},
    {10, 14, {bands5, 20}},
    {12, 13, {bands7, 21}},
};

constexpr Option options16[] = {
    {15, 16, {bands8, 21}},
    {10, 9, {bands5, 20}},
    {15, 16, {bands8, 21}},
};

constexpr Option options17[] = {
    {17, 18, {bands9, 21}},
    {11, 19, {bands6, 20}},
    {17, 18, {bands9, 21}},
};

constexpr Option options18[] = {
    {15, 16, {bands8, 21}},
    {11, 9, {bands6, 20}},
    {15, 16, {bands8, 21}},
};

constexpr Option options19[] = {
    {15, 16, {bands8, 21}},
    {8, 9, {bands4, 20}},
    {15, 16, {bands8, 21}},
};

constexpr Option options20[] = {
    {15, 16, {bands8, 21}},
    {8, 9, {bands4, 20}},
    {15, 16, {bands8, 21}},
};

constexpr Sample samples[] = {
    {"vertical_justification/allow/off/all", options0, 3, 1, true},
    {"vertical_justification/allow/off/protection", options1, 3, 0, false},
    {"vertical_justification/avoid/off/all", options2, 3, 1, true},
    {"vertical_justification/avoid/off/protection", options3, 3, 2, true},
    {"vertical_justification/publisher/off/all", options4, 3, 1, true},
    {"vertical_justification/publisher/off/protection", options5, 3, 0, false},
    {"vertical_spacing_scope/allow/center/all", options6, 2, 1, true},
    {"vertical_spacing_scope/allow/expand/all", options7, 2, 1, true},
    {"vertical_spacing_scope/allow/off/all", options8, 2, 0, false},
    {"vertical_spacing_scope/avoid/center/all", options9, 2, 1, true},
    {"vertical_spacing_scope/avoid/expand/all", options10, 2, 1, true},
    {"vertical_spacing_scope/avoid/off/all", options11, 2, 0, false},
    {"vertical_spacing_scope/publisher/center/all", options12, 2, 1, true},
    {"vertical_spacing_scope/publisher/expand/all", options13, 2, 1, true},
    {"vertical_spacing_scope/publisher/off/all", options14, 2, 0, false},
    {"widows_orphans/publisher/center/all", options15, 3, 2, true},
    {"widows_orphans/publisher/center/protection", options16, 3, 2, true},
    {"widows_orphans/publisher/expand/all", options17, 3, 2, true},
    {"widows_orphans/publisher/expand/protection", options18, 3, 2, true},
    {"widows_orphans/publisher/off/all", options19, 3, 2, true},
    {"widows_orphans/publisher/off/protection", options20, 3, 2, true},
};
}
