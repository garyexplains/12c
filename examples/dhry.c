/****************** "DHRYSTONE" Benchmark Program ***************************/
/*
 *  Dhrystone Benchmark, C, Version 2.2 (modernised).
 *
 *  Original version (in Ada, and the first C version):
 *      Reinhold P. Weicker
 *      "Communications of the ACM" vol. 27, no. 10 (Oct. 1984),
 *      pp. 1013 - 1030.
 *  C versions 1.1 and 2.1:  Rick Richardson
 *  Single-file C/2.2 (auto-compiling, two-pass):  Steven Pemberton, CWI
 *  This file is Pemberton's dry.c converted to modern ISO C: it is a single
 *  translation unit compiled in one pass, with no shell tricks and no
 *  compatibility options.
 *
 *  The statements executed inside the measurement loop are unchanged from
 *  version 2.2.
 *
 *  Modernisation summary:
 *  - ISO C function prototypes and definitions instead of K&R style; the
 *    PASS2 split and the "sh dry.c" self-compiling trick are gone.
 *  - Implicit declarations replaced by <stdlib.h>, <string.h>, <stdbool.h>,
 *    <time.h>; malloc() results are checked.
 *  - Timing uses clock() from <time.h>, which measures process CPU time,
 *    the portable modern equivalent of the Berkeley times() call used by
 *    the original.  The system-dependent HZ is replaced by CLOCKS_PER_SEC,
 *    and the TIME/MSC_CLOCK/HZ variants are gone.
 *  - double instead of float for the reported results; pointers are
 *    printed with %p instead of a cast to int.
 *  - The REG "register" experiment, NOSTRUCTASSIGN and NOENUM fallbacks are
 *    gone: register allocation is done by the compiler, structure
 *    assignment and enums are standard C.
 *  - Enum_Loc in Proc_2 and Ch_Loc in Func_2 are initialised at
 *    declaration: the original read them uninitialised on control paths
 *    that are never taken, which is undefined behaviour in modern terms.
 *
 *  Note on optimisation: as a single translation unit, modern optimisers
 *  may inline these procedures and hoist invariant code out of the
 *  measurement loop, so results are not directly comparable with the
 *  original "separate compilation, no procedure merging" ground rules.
 *  Compile without -flto, and check the generated code, when comparing.
 *
 *  Build and run:
 *      cc -O2 -o dhry dhry.c && ./dhry 1000000
 *  Results go to stdout; the final values of the benchmark variables (for
 *  correctness checking) go to stderr, so `./dhry > /dev/null` shows them.
 *
 *  Collection of Results:  Reinhold Weicker and Rick Richardson
 *  (see the original version 2.2 for full addresses and requirements).
 ***************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define Version        "C, Version 2.2 (modernised)"
#define NUMBER_OF_RUNS 50000L /* Default number of runs */

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 }
        Enumeration;
        /* for boolean and enumeration types in Ada, Pascal */

/* General definitions: */

typedef int     One_Thirty;
typedef int     One_Fifty;
typedef bool    Boolean;
typedef char    Capital_Letter;
typedef char    Str_30[31];
typedef int     Arr_1_Dim[50];
typedef int     Arr_2_Dim[50][50];

typedef struct record
    {
    struct record *Ptr_Comp;
    Enumeration    Discr;
    union {
          struct {
                  Enumeration Enum_Comp;
                  int         Int_Comp;
                  char        Str_Comp[31];
                  } var_1;
          struct {
                  Enumeration E_Comp_2;
                  char        Str_2_Comp[31];
                  } var_2;
          struct {
                  char        Ch_1_Comp;
                  char        Ch_2_Comp;
                  } var_3;
          } variant;
      } Rec_Type, *Rec_Pointer;

/*
 * Timing: clock() returns the process CPU time in units of
 * 1/CLOCKS_PER_SEC seconds.  A measurement should last at least 2 seconds.
 */
#define CLOCK_TYPE      "clock()"
#define Mic_secs_Per_Second 1000000.0
#define Too_Small_Time  (2 * CLOCKS_PER_SEC)
#define Start_Timer()   (Begin_Time = clock())
#define Stop_Timer()    (End_Time = clock())

/* Global Variables: */

Rec_Pointer     Ptr_Glob,
                Next_Ptr_Glob;
int             Int_Glob;
Boolean         Bool_Glob;
char            Ch_1_Glob,
                Ch_2_Glob;
int             Arr_1_Glob[50];
int             Arr_2_Glob[50][50];

/* Function prototypes: */

void            Proc_1(Rec_Pointer Ptr_Val_Par);
                /* executed once */
void            Proc_2(One_Fifty *Int_Par_Ref);
                /* executed once; *Int_Par_Ref == 1, becomes 4 */
void            Proc_3(Rec_Pointer *Ptr_Ref_Par);
                /* executed once; Ptr_Ref_Par becomes Ptr_Glob */
void            Proc_4(void); /* without parameters */
                /* executed once */
void            Proc_5(void); /* without parameters */
                /* executed once */
void            Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par);
                /* executed once */
void            Proc_7(One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
                       One_Fifty *Int_Par_Ref);
                /* executed three times */
void            Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
                       int Int_1_Par_Val, int Int_2_Par_Val);
                /* executed once */
Enumeration     Func_1(Capital_Letter Ch_1_Par_Val,
                       Capital_Letter Ch_2_Par_Val);
                /* executed three times */
Boolean         Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref);
                /* executed once */
Boolean         Func_3(Enumeration Enum_Par_Val);
                /* executed once */

int main(int argc, char *argv[])
/*****/
/* main program, corresponds to procedures            */
/* Main and Proc_0 in the Ada version                 */
{
  One_Fifty     Int_1_Loc = 0;
  One_Fifty     Int_2_Loc = 0;
  One_Fifty     Int_3_Loc = 0;
  char          Ch_Index = 0;
  Enumeration   Enum_Loc = Ident_1;
    /* Initialised so that the final report is well defined for any   */
    /* argument values; these are outside the measurement loop.       */
  Str_30        Str_1_Loc;
  Str_30        Str_2_Loc;
  long          Run_Index;
  long          Number_Of_Runs;
  bool          Done;
  clock_t       Begin_Time, End_Time, User_Time;
  double        Microseconds, Dhrystones_Per_Second;

  /* Arguments */
  if (argc > 2)
  {
    fprintf(stderr, "Usage: %s [number of loops]\n", argv[0]);
    return EXIT_FAILURE;
  }
  if (argc == 2)
    Number_Of_Runs = strtol(argv[1], NULL, 10);
  else
    Number_Of_Runs = NUMBER_OF_RUNS;
  if (Number_Of_Runs <= 0)
    Number_Of_Runs = NUMBER_OF_RUNS;

  /* Initializations */

  Next_Ptr_Glob = malloc(sizeof(Rec_Type));
  Ptr_Glob = malloc(sizeof(Rec_Type));
  if (Ptr_Glob == NULL || Next_Ptr_Glob == NULL)
  {
    fprintf(stderr, "Out of memory\n");
    return EXIT_FAILURE;
  }

  Ptr_Glob->Ptr_Comp                = Next_Ptr_Glob;
  Ptr_Glob->Discr                   = Ident_1;
  Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
  Ptr_Glob->variant.var_1.Int_Comp  = 40;
  strcpy(Ptr_Glob->variant.var_1.Str_Comp,
         "DHRYSTONE PROGRAM, SOME STRING");
  strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");

  Arr_2_Glob[8][7] = 10;
        /* Was missing in published program. Without this statement,    */
        /* Arr_2_Glob[8][7] would have an undefined value.               */

  printf("\n");
  printf("Dhrystone Benchmark, Version %s\n", Version);
  printf("Using %s, CLOCKS_PER_SEC=%ld\n", CLOCK_TYPE, (long)CLOCKS_PER_SEC);
  printf("\n");

  Done = false;
  while (!Done)
  {
    printf("Trying %ld runs through Dhrystone:\n", Number_Of_Runs);

    /***************/
    /* Start timer */
    /***************/

    Start_Timer();

    for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index)
    {

      Proc_5();
      Proc_4();
        /* Ch_1_Glob == 'A', Ch_2_Glob == 'B', Bool_Glob == true */
      Int_1_Loc = 2;
      Int_2_Loc = 3;
      strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
      Enum_Loc = Ident_2;
      Bool_Glob = ! Func_2(Str_1_Loc, Str_2_Loc);
        /* Bool_Glob == 1 */
      while (Int_1_Loc < Int_2_Loc)  /* loop body executed once */
      {
        Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
          /* Int_3_Loc == 7 */
        Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
          /* Int_3_Loc == 7 */
        Int_1_Loc += 1;
      } /* while */
        /* Int_1_Loc == 3, Int_2_Loc == 3, Int_3_Loc == 7 */
      Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
        /* Int_Glob == 5 */
      Proc_1(Ptr_Glob);
      for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index)
                               /* loop body executed twice */
      {
        if (Enum_Loc == Func_1(Ch_Index, 'C'))
            /* then, not executed */
          {
          Proc_6(Ident_1, &Enum_Loc);
          strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
          Int_2_Loc = (int) Run_Index;
          Int_Glob = (int) Run_Index;
          }
      }
        /* Int_1_Loc == 3, Int_2_Loc == 3, Int_3_Loc == 7 */
      Int_2_Loc = Int_2_Loc * Int_1_Loc;
      Int_1_Loc = Int_2_Loc / Int_3_Loc;
      Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
        /* Int_1_Loc == 1, Int_2_Loc == 13, Int_3_Loc == 7 */
      Proc_2(&Int_1_Loc);
        /* Int_1_Loc == 5 */

    } /* loop "for Run_Index" */

    /**************/
    /* Stop timer */
    /**************/

    Stop_Timer();

    User_Time = End_Time - Begin_Time;

    if (User_Time < Too_Small_Time)
    {
      printf("Measured time too small to obtain meaningful results\n");
      Number_Of_Runs = Number_Of_Runs * 10;
      printf("\n");
    }
    else Done = true;
  }

  fprintf(stderr, "Final values of the variables used in the benchmark:\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Int_Glob:            %d\n", Int_Glob);
  fprintf(stderr, "        should be:   %d\n", 5);
  fprintf(stderr, "Bool_Glob:           %d\n", (int) Bool_Glob);
  fprintf(stderr, "        should be:   %d\n", 1);
  fprintf(stderr, "Ch_1_Glob:           %c\n", Ch_1_Glob);
  fprintf(stderr, "        should be:   %c\n", 'A');
  fprintf(stderr, "Ch_2_Glob:           %c\n", Ch_2_Glob);
  fprintf(stderr, "        should be:   %c\n", 'B');
  fprintf(stderr, "Arr_1_Glob[8]:       %d\n", Arr_1_Glob[8]);
  fprintf(stderr, "        should be:   %d\n", 7);
  fprintf(stderr, "Arr_2_Glob[8][7]:    %d\n", Arr_2_Glob[8][7]);
  fprintf(stderr, "        should be:   Number_Of_Runs + 10\n");
  fprintf(stderr, "Ptr_Glob->\n");
  fprintf(stderr, "  Ptr_Comp:          %p\n", (void *) Ptr_Glob->Ptr_Comp);
  fprintf(stderr, "        should be:   (implementation-dependent)\n");
  fprintf(stderr, "  Discr:             %d\n", Ptr_Glob->Discr);
  fprintf(stderr, "        should be:   %d\n", 0);
  fprintf(stderr, "  Enum_Comp:         %d\n",
                                Ptr_Glob->variant.var_1.Enum_Comp);
  fprintf(stderr, "        should be:   %d\n", 2);
  fprintf(stderr, "  Int_Comp:          %d\n", Ptr_Glob->variant.var_1.Int_Comp);
  fprintf(stderr, "        should be:   %d\n", 17);
  fprintf(stderr, "  Str_Comp:          %s\n", Ptr_Glob->variant.var_1.Str_Comp);
  fprintf(stderr, "        should be:   DHRYSTONE PROGRAM, SOME STRING\n");
  fprintf(stderr, "Next_Ptr_Glob->\n");
  fprintf(stderr, "  Ptr_Comp:          %p\n",
                                (void *) Next_Ptr_Glob->Ptr_Comp);
  fprintf(stderr, "        should be:   (implementation-dependent), same as above\n");
  fprintf(stderr, "  Discr:             %d\n", Next_Ptr_Glob->Discr);
  fprintf(stderr, "        should be:   %d\n", 0);
  fprintf(stderr, "  Enum_Comp:         %d\n",
                                Next_Ptr_Glob->variant.var_1.Enum_Comp);
  fprintf(stderr, "        should be:   %d\n", 1);
  fprintf(stderr, "  Int_Comp:          %d\n",
                                Next_Ptr_Glob->variant.var_1.Int_Comp);
  fprintf(stderr, "        should be:   %d\n", 18);
  fprintf(stderr, "  Str_Comp:          %s\n",
                                Next_Ptr_Glob->variant.var_1.Str_Comp);
  fprintf(stderr, "        should be:   DHRYSTONE PROGRAM, SOME STRING\n");
  fprintf(stderr, "Int_1_Loc:           %d\n", Int_1_Loc);
  fprintf(stderr, "        should be:   %d\n", 5);
  fprintf(stderr, "Int_2_Loc:           %d\n", Int_2_Loc);
  fprintf(stderr, "        should be:   %d\n", 13);
  fprintf(stderr, "Int_3_Loc:           %d\n", Int_3_Loc);
  fprintf(stderr, "        should be:   %d\n", 7);
  fprintf(stderr, "Enum_Loc:            %d\n", Enum_Loc);
  fprintf(stderr, "        should be:   %d\n", 1);
  fprintf(stderr, "Str_1_Loc:           %s\n", Str_1_Loc);
  fprintf(stderr, "        should be:   DHRYSTONE PROGRAM, 1'ST STRING\n");
  fprintf(stderr, "Str_2_Loc:           %s\n", Str_2_Loc);
  fprintf(stderr, "        should be:   DHRYSTONE PROGRAM, 2'ND STRING\n");
  fprintf(stderr, "\n");

  free(Ptr_Glob);
  free(Next_Ptr_Glob);

  Microseconds = (double) User_Time * Mic_secs_Per_Second
                      / ((double) CLOCKS_PER_SEC * ((double) Number_Of_Runs));
  Dhrystones_Per_Second = ((double) CLOCKS_PER_SEC * (double) Number_Of_Runs)
                      / (double) User_Time;

  printf("Microseconds for one run through Dhrystone: ");
  printf("%10.1f \n", Microseconds);
  printf("Dhrystones per Second:                      ");
  printf("%10.0f \n", Dhrystones_Per_Second);
  printf("\n");

  return 0;
}


void Proc_1(Rec_Pointer Ptr_Val_Par)
/******************/
    /* executed once */
{
  Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
                                        /* == Ptr_Glob_Next */
  /* Local variable, initialized with Ptr_Val_Par->Ptr_Comp,    */
  /* corresponds to "rename" in Ada, "with" in Pascal           */

  *Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
  Ptr_Val_Par->variant.var_1.Int_Comp = 5;
  Next_Record->variant.var_1.Int_Comp
        = Ptr_Val_Par->variant.var_1.Int_Comp;
  Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
  Proc_3(&Next_Record->Ptr_Comp);
    /* Ptr_Val_Par->Ptr_Comp->Ptr_Comp
                        == Ptr_Glob->Ptr_Comp */
  if (Next_Record->Discr == Ident_1)
    /* then, executed */
  {
    Next_Record->variant.var_1.Int_Comp = 6;
    Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp,
           &Next_Record->variant.var_1.Enum_Comp);
    Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
    Proc_7(Next_Record->variant.var_1.Int_Comp, 10,
           &Next_Record->variant.var_1.Int_Comp);
  }
  else /* not executed */
    *Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
} /* Proc_1 */


void Proc_2(One_Fifty *Int_Par_Ref)
/******************/
    /* executed once */
    /* *Int_Par_Ref == 1, becomes 4 */
{
  One_Fifty    Int_Loc;
  Enumeration  Enum_Loc = Ident_1;
    /* Initialised so that the loop is well defined on the        */
    /* not-taken path (the original read it uninitialised there). */

  Int_Loc = *Int_Par_Ref + 10;
  do /* executed once */
    if (Ch_1_Glob == 'A')
      /* then, executed */
    {
      Int_Loc -= 1;
      *Int_Par_Ref = Int_Loc - Int_Glob;
      Enum_Loc = Ident_1;
    } /* if */
  while (Enum_Loc != Ident_1); /* true */
} /* Proc_2 */


void Proc_3(Rec_Pointer *Ptr_Ref_Par)
/******************/
    /* executed once */
    /* Ptr_Ref_Par becomes Ptr_Glob */
{
  if (Ptr_Glob != NULL)
    /* then, executed */
    *Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
  Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
} /* Proc_3 */


void Proc_4(void) /* without parameters */
/*******/
    /* executed once */
{
  Boolean Bool_Loc;

  Bool_Loc = Ch_1_Glob == 'A';
  Bool_Glob = Bool_Loc | Bool_Glob;
  Ch_2_Glob = 'B';
} /* Proc_4 */


void Proc_5(void) /* without parameters */
/*******/
    /* executed once */
{
  Ch_1_Glob = 'A';
  Bool_Glob = false;
} /* Proc_5 */


void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par)
/*********************************/
    /* executed once */
    /* Enum_Val_Par == Ident_3, Enum_Ref_Par becomes Ident_2 */
{
  *Enum_Ref_Par = Enum_Val_Par;
  if (! Func_3(Enum_Val_Par))
    /* then, not executed */
    *Enum_Ref_Par = Ident_4;
  switch (Enum_Val_Par)
  {
    case Ident_1:
      *Enum_Ref_Par = Ident_1;
      break;
    case Ident_2:
      if (Int_Glob > 100)
        /* then */
      *Enum_Ref_Par = Ident_1;
      else *Enum_Ref_Par = Ident_4;
      break;
    case Ident_3: /* executed */
      *Enum_Ref_Par = Ident_2;
      break;
    case Ident_4: break;
    case Ident_5:
      *Enum_Ref_Par = Ident_3;
      break;
  } /* switch */
} /* Proc_6 */


void Proc_7(One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
            One_Fifty *Int_Par_Ref)
/**********************************************/
    /* executed three times                                       */
    /* first call:      Int_1_Par_Val == 2, Int_2_Par_Val == 3,   */
    /*                  Int_Par_Ref becomes 7                     */
    /* second call:     Int_1_Par_Val == 10, Int_2_Par_Val == 5,  */
    /*                  Int_Par_Ref becomes 17                    */
    /* third call:      Int_1_Par_Val == 6, Int_2_Par_Val == 10,  */
    /*                  Int_Par_Ref becomes 18                    */
{
  One_Fifty Int_Loc;

  Int_Loc = Int_1_Par_Val + 2;
  *Int_Par_Ref = Int_2_Par_Val + Int_Loc;
} /* Proc_7 */


void Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
            int Int_1_Par_Val, int Int_2_Par_Val)
/*********************************************************************/
    /* executed once      */
    /* Int_Par_Val_1 == 3 */
    /* Int_Par_Val_2 == 7 */
{
  One_Fifty Int_Index;
  One_Fifty Int_Loc;

  Int_Loc = Int_1_Par_Val + 5;
  Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
  Arr_1_Par_Ref[Int_Loc+1] = Arr_1_Par_Ref[Int_Loc];
  Arr_1_Par_Ref[Int_Loc+30] = Int_Loc;
  for (Int_Index = Int_Loc; Int_Index <= Int_Loc+1; ++Int_Index)
    Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
  Arr_2_Par_Ref[Int_Loc][Int_Loc-1] += 1;
  Arr_2_Par_Ref[Int_Loc+20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
  Int_Glob = 5;
} /* Proc_8 */


Enumeration Func_1(Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val)
/*************************************************/
    /* executed three times                                         */
    /* first call:      Ch_1_Par_Val == 'H', Ch_2_Par_Val == 'R'    */
    /* second call:     Ch_1_Par_Val == 'A', Ch_2_Par_Val == 'C'    */
    /* third call:      Ch_1_Par_Val == 'B', Ch_2_Par_Val == 'C'    */
{
  Capital_Letter  Ch_1_Loc;
  Capital_Letter  Ch_2_Loc;

  Ch_1_Loc = Ch_1_Par_Val;
  Ch_2_Loc = Ch_1_Loc;
  if (Ch_2_Loc != Ch_2_Par_Val)
    /* then, executed */
    return Ident_1;
  else /* not executed */
  {
    Ch_1_Glob = Ch_1_Loc;
    return Ident_2;
  }
} /* Func_1 */


Boolean Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref)
/*************************************************/
    /* executed once */
    /* Str_1_Par_Ref == "DHRYSTONE PROGRAM, 1'ST STRING" */
    /* Str_2_Par_Ref == "DHRYSTONE PROGRAM, 2'ND STRING" */
{
  One_Thirty     Int_Loc;
  Capital_Letter Ch_Loc = ' ';
    /* Initialised so that the not-taken control paths are well    */
    /* defined (the original read it uninitialised there).         */

  Int_Loc = 2;
  while (Int_Loc <= 2) /* loop body executed once */
    if (Func_1(Str_1_Par_Ref[Int_Loc],
               Str_2_Par_Ref[Int_Loc+1]) == Ident_1)
      /* then, executed */
    {
      Ch_Loc = 'A';
      Int_Loc += 1;
    } /* if, while */
  if (Ch_Loc >= 'W' && Ch_Loc < 'Z')
    /* then, not executed */
    Int_Loc = 7;
  if (Ch_Loc == 'R')
    /* then, not executed */
    return true;
  else /* executed */
  {
    if (strcmp(Str_1_Par_Ref, Str_2_Par_Ref) > 0)
      /* then, not executed */
    {
      Int_Loc += 7;
      Int_Glob = Int_Loc;
      return true;
    }
    else /* executed */
      return false;
  } /* if Ch_Loc */
} /* Func_2 */


Boolean Func_3(Enumeration Enum_Par_Val)
/***************************/
    /* executed once        */
    /* Enum_Par_Val == Ident_3 */
{
  Enumeration Enum_Loc;

  Enum_Loc = Enum_Par_Val;
  if (Enum_Loc == Ident_3)
    /* then, executed */
    return true;
  else /* not executed */
    return false;
} /* Func_3 */
