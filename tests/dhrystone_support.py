"""Temporary integer-core integration fixture, not unchanged-source completion.

Keep the benchmark's loop, procedures, initialization and final-value report.
Disable adaptive retries and move only floating-point reporting into host C.
All replacements are checked so source changes cannot silently weaken the test.
"""


def integer_fixture(original):
    def replace_once(source, old, new):
        if source.count(old) != 1:
            raise AssertionError("Dhrystone fixture marker changed: " + old)
        return source.replace(old, new, 1)

    declarations = "  double        Microseconds, Dhrystones_Per_Second;"
    source = replace_once(original, declarations, "")
    retry = '''    if (User_Time < Too_Small_Time)
    {
      printf("Measured time too small to obtain meaningful results\\n");
      Number_Of_Runs = Number_Of_Runs * 10;
      printf("\\n");
    }
    else Done = true;'''
    source = replace_once(source, retry, "    Done = true;")
    start = source.index("  Microseconds = (double) User_Time")
    end = source.index("  return 0;", start)
    reporting = source[start:end]
    source = replace_once(source, reporting,
                          "  dhrystone_report(User_Time, Number_Of_Runs);\n\n")
    source = replace_once(source, "int main(int argc, char *argv[])",
                          "void dhrystone_report(long ticks, long runs);\n"
                          "int main(int argc, char *argv[])")
    helper = ("#include <stdio.h>\n#include <time.h>\n"
              "#define Mic_secs_Per_Second 1000000.0\n"
              "void dhrystone_report(long User_Time, long Number_Of_Runs) {\n" +
              declarations + "\n" + reporting + "}\n")
    return source, helper
