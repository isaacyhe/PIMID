/*
 * Dhrystone 2.1 -- Simplified standalone single-threaded benchmark
 * Classic integer benchmark: struct assignments, enum comparisons,
 * string operations, function calls.
 *
 * Compile: g++ -O2 -I<path-to-zsim/misc/hooks> dhrystone.c -o dhrystone
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsim_hooks.h"

/* ------------------------------------------------------------------ */
/*  Arg parser                                                        */
/* ------------------------------------------------------------------ */
static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* ------------------------------------------------------------------ */
/*  Dhrystone types and globals                                       */
/* ------------------------------------------------------------------ */
typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;

typedef struct Record {
    struct Record* Ptr_Comp;
    Enumeration    Discr;
    union {
        struct {
            Enumeration Enum_Comp;
            int         Int_Comp;
            char        Str_Comp[31];
        } var_1;
    } variant;
} Rec_Type, *Rec_Pointer;

static int       Int_Glob;
static int       Bool_Glob;
static char      Ch_1_Glob;
static char      Ch_2_Glob;
static int       Arr_1_Glob[50];
static int       Arr_2_Glob[50][50];
static Rec_Type  Rec_Glob, Next_Rec_Glob;

/* ------------------------------------------------------------------ */
/*  Helper functions (simplified Dhrystone procedures)                 */
/* ------------------------------------------------------------------ */
static Enumeration Func_1(char Ch_1_Par, char Ch_2_Par) {
    char Ch_1_Loc = Ch_1_Par;
    char Ch_2_Loc = Ch_1_Loc;
    if (Ch_2_Loc != Ch_2_Par)
        return Ident_1;
    else {
        Ch_1_Glob = Ch_1_Loc;
        return Ident_2;
    }
}

static int Func_2(const char* Str_1_Par, const char* Str_2_Par) {
    int Int_Loc = 2;
    char Ch_Loc = 'A';
    while (Int_Loc <= 2) {
        if (Func_1(Str_1_Par[Int_Loc], Str_2_Par[Int_Loc + 1]) == Ident_1) {
            Ch_Loc = 'A';
            Int_Loc++;
        }
    }
    if (Ch_Loc >= 'W' && Ch_Loc < 'Z')
        Int_Loc = 7;
    if (Ch_Loc == 'R')
        return 1;
    else {
        if (strcmp(Str_1_Par, Str_2_Par) > 0) {
            Int_Loc += 7;
            return 1;
        } else
            return 0;
    }
}

static int Func_3(Enumeration Enum_Par) {
    Enumeration Enum_Loc = Enum_Par;
    if (Enum_Loc == Ident_3) return 1;
    return 0;
}

static void Proc_6(Enumeration Enum_Val_Par, Enumeration* Enum_Ref_Par) {
    *Enum_Ref_Par = Enum_Val_Par;
    if (!Func_3(Enum_Val_Par))
        *Enum_Ref_Par = Ident_4;
    switch (Enum_Val_Par) {
        case Ident_1: *Enum_Ref_Par = Ident_1; break;
        case Ident_2: if (Int_Glob > 100) *Enum_Ref_Par = Ident_1;
                      else *Enum_Ref_Par = Ident_4; break;
        case Ident_3: *Enum_Ref_Par = Ident_2; break;
        case Ident_4: break;
        case Ident_5: *Enum_Ref_Par = Ident_3; break;
    }
}

static void Proc_7(int Int_1_Par, int Int_2_Par, int* Int_Par_Ref) {
    int Int_Loc = Int_1_Par + 2;
    *Int_Par_Ref = Int_2_Par + Int_Loc;
}

static void Proc_8(int Arr_1_Par[], int Arr_2_Par[][50], int Int_1_Par, int Int_2_Par) {
    int Int_Loc = Int_1_Par + 5;
    Arr_1_Par[Int_Loc] = Int_2_Par;
    Arr_1_Par[Int_Loc + 1] = Arr_1_Par[Int_Loc];
    Arr_1_Par[Int_Loc + 30] = Int_Loc;
    for (int Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; Int_Index++)
        Arr_2_Par[Int_Loc][Int_Index] = Int_Loc;
    Arr_2_Par[Int_Loc][Int_Loc - 1] += 1;
    Arr_2_Par[Int_Loc + 20][Int_Loc] = Arr_1_Par[Int_Loc];
    Int_Glob = 5;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    int num_iters = parse_int_arg(argc, argv, "--iters", 10000);

    printf("Dhrystone 2.1 (simplified) -- iters=%d\n", num_iters);

    /* Initialisation */
    Rec_Glob.Ptr_Comp                = &Next_Rec_Glob;
    Rec_Glob.Discr                   = Ident_1;
    Rec_Glob.variant.var_1.Enum_Comp = Ident_3;
    Rec_Glob.variant.var_1.Int_Comp  = 40;
    strcpy(Rec_Glob.variant.var_1.Str_Comp,
           "DHRYSTONE PROGRAM, SOME STRING");

    Next_Rec_Glob = Rec_Glob;  /* deep copy */
    Next_Rec_Glob.Ptr_Comp = &Rec_Glob;

    char Str_1_Loc[31], Str_2_Loc[31];
    strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");
    Arr_2_Glob[8][7] = 10;

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    for (int Run_Index = 1; Run_Index <= num_iters; Run_Index++) {
        Proc_8(Arr_1_Glob, Arr_2_Glob, Int_Glob, Ch_1_Glob);

        Enumeration Enum_Loc;
        Proc_6(Rec_Glob.variant.var_1.Enum_Comp, &Enum_Loc);

        int Int_1_Loc = 2, Int_2_Loc = 3, Int_3_Loc;
        strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");

        Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);

        while (Int_1_Loc < Int_2_Loc) {
            Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
            Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
            Int_1_Loc += 1;
        }

        Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);

        Proc_6(Ident_1, &Enum_Loc);

        Ch_1_Glob = 'A';
        Ch_2_Glob = 'B';
    }

    zsim_roi_end();
    /* ---- ROI end ---- */

    long checksum = (long)Int_Glob + (long)Bool_Glob +
                    (long)Ch_1_Glob + (long)Ch_2_Glob +
                    (long)Arr_2_Glob[8][7];

    printf("Int_Glob=%d  Bool_Glob=%d  Ch_1_Glob=%c  Ch_2_Glob=%c  Arr_2_Glob[8][7]=%d\n",
           Int_Glob, Bool_Glob, Ch_1_Glob, Ch_2_Glob, Arr_2_Glob[8][7]);
    printf("BENCH_CHECKSUM: %ld\n", checksum);
    printf("BENCH_DONE\n");

    return 0;
}
