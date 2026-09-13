"""Frontend checks, assembly audits, and native AArch64 execution tests."""

import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "build" / "4c"
ALLOWED = {"ldr", "str", "ldrb", "strb", "add", "sub", "cbz", "tbz",
           "bl", "blr", "ret", "adrp"}
NATIVE = platform.machine().lower() in {"aarch64", "arm64"}
TARGET = "macos" if platform.system() == "Darwin" else "linux"


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], capture_output=True,
                          timeout=20, **kwargs)


class CompilerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="4c-test-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def compile(self, source, target=TARGET, extra=()):
        path = self.work / "input.c"
        path.write_text(source)
        asm = self.work / "output.s"
        result = run([COMPILER, "--target", target, *extra, "-o", asm, path])
        return result, asm

    def successful(self, source, target=TARGET, extra=()):
        result, asm = self.compile(source, target, extra)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.audit_source(asm.read_text())
        return asm

    def audit_source(self, assembly):
        instructions = []
        for line in assembly.splitlines():
            line = line.strip()
            if not line or line.startswith((".", "//")) or line.endswith(":"):
                continue
            instructions.append(line.split()[0])
        self.assertTrue(instructions)
        self.assertLessEqual(set(instructions), ALLOWED)

    def execute(self, source, expected_output=b"", expected_status=0, helper=None, helper_c=False):
        asm = self.successful(source)
        binary = self.work / "program"
        cc = shlex.split(os.environ.get("TEST_CC", "cc"))
        inputs = [asm]
        if helper is not None:
            helper_path = self.work / ("helper.c" if helper_c else "helper.s")
            helper_path.write_text(helper)
            inputs.append(helper_path)
        link = run([*cc, *inputs, "-o", binary])
        self.assertEqual(link.returncode, 0, link.stderr.decode())
        result = run([binary])
        self.assertEqual(result.stdout, expected_output)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(result.returncode, expected_status)

    def test_example_both_assembly_targets(self):
        source = (ROOT / "examples" / "hello.c").read_text()
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                self.assertIn("bl _putchar" if target == "macos" else "bl putchar", text)
                self.assertIn(".globl _main" if target == "macos" else ".globl main", text)
                self.assertEqual(".note.GNU-stack" in text, target == "linux")

    def test_include_directory_is_used(self):
        headers = self.work / "headers"
        headers.mkdir()
        (headers / "stdio.h").write_text("int custom(int value);\n")
        self.successful("#include <stdio.h>\nint main(){return custom(5);}",
                        extra=("-I", headers))
        result, _ = self.compile("#include <stdio.h>\nint main(){return putchar(65);}",
                                 extra=("-I", headers))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"undeclared function", result.stderr)

    def test_comments_repeated_header_and_stdout(self):
        source = """/* comment */
        #include <stdio.h>
        #include <stdio.h>
        int main(void) { // line comment
          int a = (+65); putchar(a); return 0;
        }
        """
        self.successful(source)
        result = run([COMPILER, self.work / "input.c"])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.audit_source(result.stdout.decode())

    def test_rejects_unsupported_or_invalid_input(self):
        cases = [
            ("int main(){putchar(65);return 0;}", "undeclared function"),
            ("int main(){return missing;}", "unknown local"),
            ("int main(){int a=1;int a=2;return 0;}", "duplicate local"),
            ("int main(){int a=a;return 0;}", "unknown local"),
            ("int main(){1=2;}", "assignment requires a local"),
            ("int main(){int a=0; a+1=2;}", "assignment requires a local"),
            ("int main(){int a=0; +a=2;}", "assignment requires a local"),
            ("int main(){int a=0; (a=1)=2;}", "assignment requires a local"),
            ("int f(int); int main(){f(1)=2;}", "assignment requires a local"),
            ("int main(){missing=1;}", "unknown local"),
            ("int main(){int a=0; a=;}", "integer expression"),
            ("int main(){return 1+;}", "integer expression"),
            ("int main(){return 0x41;}", "decimal integer"),
            ("int main(){return 065;}", "decimal integer"),
            ("int main(){return 2147483648;}", "decimal integer"),
            ("int main(){return 99999999999999999999999;}", "decimal integer"),
            ("int main(){return ++1;}", "increment and decrement"),
            ("int main(){return --1;}", "increment and decrement"),
            ("int main(){int while=0;return 0;}", "identifier"),
            ("int main(){return 0;", "expected '}'"),
            ("int main(){{int a=1;}return a;}", "unknown local"),
            ("int main(){int a=1;{int a=a;}return 0;}", "self-initializer"),
            ("int main(){if(1) int a=1;}", "integer expression"),
            ("int main(){if() return 1;}", "integer expression"),
            ("int main(){else return 1;}", "integer expression"),
            ("int main(){return 1<;}", "integer expression"),
            ("int main(){int a=0;(a<1)=2;}", "assignment requires a local"),
            ("int main(){return 0;} garbage", "expected 'int'"),
            ("int main(){return 0;} int main(){return 0;}", "duplicate main"),
            ("int putchar(int);", "main() definition"),
            ("#include <stdlib.h>\nint main(){return 0;}", "only #include"),
            ("#define A 65\nint main(){return 0;}", "only #include"),
            ("/* unfinished", "unterminated comment"),
            ("int main(){return 0;}\x00", "NUL byte"),
            ("int f(int); int main(){return f();}", "wrong number"),
            ("int f(int); int main(){return f(1,2);}", "wrong number"),
            ("int f(int); int main(){int f=1;return f(2);}", "not a function"),
            ("int f(int); int f(int,int); int main(){}", "conflicting function"),
            ("int f(int); int f(void){return 0;} int main(){}", "conflicting function"),
            ("int f(){return 1;} int f(){return 2;} int main(){}", "duplicate function"),
            ("int f(int a,int a){return a;} int main(){}", "duplicate parameter"),
            ("int f(int){return 1;} int main(){}", "require parameter names"),
            ("int f(int a){int a=1;return a;} int main(){}", "duplicate local"),
            ("int f(int a){return a;} int main(){return a;}", "unknown local"),
            ("int f(int,int,int,int,int,int,int,int,int); int main(){}", "at most eight"),
            ("int main(int a){return a;}", "main parameters"),
            ("int f(); int main(){}", "use (void)"),
            ("int main(){return f(1);} int f(int a){return a;}", "undeclared function"),
        ]
        for source, diagnostic in cases:
            with self.subTest(source=source):
                output = self.work / "output.s"
                output.write_text("existing output\n")
                result, _ = self.compile(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic.encode(), result.stderr)
                self.assertRegex(result.stderr.decode(), r"input\.c:\d+:")
                self.assertEqual(output.read_text(), "existing output\n")

    def test_cli_errors(self):
        for args in ([], ["--target", "unknown"], ["--target"], ["-o"],
                     ["-I"], ["--unknown"], [self.work / "missing.c"]):
            with self.subTest(args=args):
                self.assertNotEqual(run([COMPILER, *args]).returncode, 0)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_user_program(self):
        self.execute((ROOT / "examples" / "hello.c").read_text(), b"A")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_locals_survive_calls(self):
        self.execute("""#include <stdio.h>
            int main(){int a=65;int b=66;int c=a;
            putchar(b);putchar(a);putchar(c);return 7;}
            """, b"BAA", 7)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_return_values_and_nested_calls(self):
        self.execute("#include <stdio.h>\nint main(){return putchar(65);}", b"A", 65)
        self.execute("#include <stdio.h>\nint main(){putchar(putchar(65));}", b"AA")
        self.execute("int main(void){return -7;}", expected_status=249)
        self.execute("int main(){return 2147483647;}", expected_status=255)
        self.execute("int main(){return 0;return 4;}")
        self.execute("int main(){}")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_largest_frame(self):
        declarations = "".join(f"int a{i}={i};" for i in range(256))
        self.execute("int main(){" + declarations + "return a255;}", expected_status=255)

    def test_arithmetic_both_assembly_targets(self):
        source = (ROOT / "examples" / "arithmetic.c").read_text()
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                self.successful(source, target)

    def test_source_comments_precede_generated_statements(self):
        source = """#include <stdio.h>
int main() {
    int a = 60;
    a = a +
        5;
    putchar(a); return 0;
}
"""
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                self.assertIn("// C line 2: int main() {\n", text)
                self.assertIn("// C line 3: int a = 60;\n    ldr", text)
                self.assertIn("// C line 4: a = a + 5;\n    add", text)
                self.assertIn("// C line 6: putchar(a);\n    sub sp", text)
                self.assertIn("// C line 6: return 0;\n    ldr", text)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_arithmetic_example(self):
        self.execute((ROOT / "examples" / "arithmetic.c").read_text(), b"A")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_precedence_and_boundaries(self):
        cases = {
            "20 - 5 - 3": 12,
            "20 - (5 - 3)": 18,
            "-5 + 9": 4,
            "-(5 + 9)": -14,
            "10 - -3": 13,
            "10 + -3": 7,
            "1 + (2 + (3 + (4 + 5)))": 15,
            "2147483647 - 2147483646": 1,
            "(-2147483647 - 1) + 2147483647": -1,
            "(-2147483647 - 1) - (-2147483647)": -1,
        }
        for expression, value in cases.items():
            with self.subTest(expression=expression):
                self.execute(f"int main(){{return {expression};}}",
                             expected_status=value % 256)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_assignment_values(self):
        self.execute("""#include <stdio.h>
            int main(){int a=0;int b=0;a=b=65;putchar(a);putchar(b);
            (a)=b+1;putchar(a);int c=(b=67);putchar(c);
            putchar(b=68);return a+b+c;}
            """, b"AABCD", 201)
        self.execute("int main(){int a=1;return a=7;}", expected_status=7)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_arithmetic_across_calls(self):
        self.execute("""#include <stdio.h>
            int main(){int a=5;int b=10;
            int c=a+(b+putchar(66));putchar(c);return c-81;}
            """, b"BQ")
        # Force the external callee to overwrite caller-saved scratch registers.
        symbol = "_bump" if TARGET == "macos" else "bump"
        helper = f".text\n.p2align 2\n.globl {symbol}\n{symbol}:\n"
        helper += "".join(f"    sub x{i}, x{i}, x{i}\n" for i in range(1, 18))
        helper += "    add w0, w0, #1\n    ret\n"
        if TARGET == "linux":
            helper += '.section .note.GNU-stack,"",%progbits\n'
        self.execute("""int bump(int);
            int main(){int a=10;int b=20;
            a=a+(b+bump(3));return a+bump(b+1);}
            """, expected_status=56, helper=helper)

    def test_control_flow_assembly_targets_and_comments(self):
        source = "int main(){int a=1;while(a<3){if(a==1) a=a+1;else a=3;}return 0;}"
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                self.assertIn("// C line 1: while(a<3)\n", text)
                self.assertIn("// C line 1: if(a==1)\n", text)
                self.assertIn("// C line 1: else\n", text)
                self.assertIn("tbz", text)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_signed_comparison_matrix(self):
        import operator
        values = [-2147483648, -2147483647, -1, 0, 1, 2147483646, 2147483647]
        operators = {"<": operator.lt, "<=": operator.le, ">": operator.gt,
                     ">=": operator.ge, "==": operator.eq, "!=": operator.ne}
        def c_int(value):
            return "(-2147483647 - 1)" if value == -2147483648 else str(value)
        for op, compare in operators.items():
            checks = []
            for a in values:
                for b in values:
                    expected = int(compare(a, b))
                    checks.append(f"if (({c_int(a)} {op} {c_int(b)}) != {expected}) "
                                  f"return {len(checks) + 1};")
            with self.subTest(operator=op):
                self.execute("int main(){" + "".join(checks) + "return 0;}")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_comparison_precedence_and_call_results(self):
        for expr, expected in [("3 < 2 < 1", 1), ("2 == 1 < 3", 0),
                               ("(2 == 1) < 3", 1), ("1 < 2 == 3 > 2", 1),
                               ("1 + 2 > 4 - 2", 1), ("1 == 2 == 0", 1)]:
            with self.subTest(expression=expr):
                self.execute(f"int main(){{int a=0;return a={expr};}}",
                             expected_status=expected)
        self.execute("#include <stdio.h>\nint main(){return 65==putchar(65);}", b"A", 1)
        self.execute("#include <stdio.h>\nint main(){return -1<putchar(65);}", b"A", 1)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_branches_and_scopes(self):
        self.execute("""#include <stdio.h>
            int main(){int a=65;
            if(a>=65){putchar(a);}else{putchar(63);}
            {int a=66;putchar(a);{int a=67;putchar(a);}putchar(a);}
            putchar(a);if(0)putchar(88);else if(-1)putchar(68);
            if(1)if(0)putchar(88);else putchar(69);
            if(0){return 7;} {;} return 0;}
            """, b"ABCB ADE".replace(b" ", b""))
        # Sibling blocks reuse stack slots; 256 is the active-local limit.
        self.execute("int main(){" + "{int a=1;}" * 300 + "return 0;}")
        # An inner local may shadow a function without hiding it after block exit.
        self.execute("#include <stdio.h>\nint main(){{int putchar=1;}putchar(65);}", b"A")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_while_loops(self):
        self.execute((ROOT / "examples" / "alphabet.c").read_text(),
                     b"ABCDEFGHIJKLMNOPQRSTUVWXYZ\n")
        self.execute("""int main(){int i=0;int n=0;while(i<3){int j=0;
            while(j<4){n=n+1;j=j+1;}i=i+1;}return n;}
            """, expected_status=12)
        self.execute("int main(){int a=7;while(0)a=99;while(a>0)a=a-1;return a;}")
        self.execute("int main(){while(1){return 9;}return 1;}", expected_status=9)
        self.execute("int main(){int a=3;while(a=a-1);return a;}")

    def test_functions_both_targets(self):
        source = (ROOT / "examples" / "recursion.c").read_text()
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                prefix = "_" if target == "macos" else ""
                self.assertIn(f".globl {prefix}sum", text)
                self.assertIn(f"bl {prefix}sum", text)
                self.assertIn("// C line 3: int sum(int n) {", text)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_recursion_and_mutual_recursion(self):
        self.execute((ROOT / "examples" / "recursion.c").read_text(), b"A")
        self.execute("""int fib(int n){if(n<2)return n;
            return fib(n-1)+fib(n-2);} int main(){return fib(10);}
            """, expected_status=55)
        self.execute("""int odd(int); int even(int);
            int odd(int n){if(n==0)return 0;return even(n-1);}
            int even(int n){if(n==0)return 1;return odd(n-1);}
            int main(){return even(100)+odd(101);}
            """, expected_status=2)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_parameters_and_zero_argument_functions(self):
        self.execute("""int value(void); int value(void);
            int change(int a){a=a+1;{int a=99;}return a;}
            int main(){int a=8;int b=change(a);return a+b+value();}
            int value(){return 5;}
            """, expected_status=22)
        self.execute("""int down(int n,int acc){if(n==0)return acc;
            return down(n-1,acc+n);} int main(){return down(20,0);}
            """, expected_status=210)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_eight_arguments_and_nested_calls_native_abi(self):
        source = """#include <stdio.h>
            int host8(int,int,int,int,int,int,int,int);
            int generated8(int a,int b,int c,int d,int e,int f,int g,int h){
                if(a!=-1)return 1;if(b!=2)return 2;if(c!=-3)return 3;
                if(d!=4)return 4;if(e!=-5)return 5;if(f!=6)return 6;
                if(g!=-7)return 7;if(h!=8)return 8;return 93;}
            int main(){return host8(putchar(65),putchar(66),putchar(67),
                putchar(68),putchar(69),putchar(70),putchar(71),putchar(72));}
            """
        helper = """extern int generated8(int,int,int,int,int,int,int,int);
            int host8(int a,int b,int c,int d,int e,int f,int g,int h){
                if(a!=65 || b!=66 || c!=67 || d!=68 || e!=69 || f!=70 ||
                   g!=71 || h!=72) return 99;
                return generated8(-1,2,-3,4,-5,6,-7,8);
            }"""
        self.execute(source, b"ABCDEFGH", 93, helper=helper, helper_c=True)
        # Generated caller and callee, with more than one live argument staging area.
        self.execute("""int add(int a,int b){return a+b;}
            int main(){return add(add(1,2),add(add(3,4),add(5,6)));}
            """, expected_status=21)

    def test_pointer_targets_and_byte_conventions(self):
        source = (ROOT / "examples" / "strings.c").read_text()
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                self.assertIn("ldrb", text)
                self.assertIn("adrp x0, Lstr0", text)
                self.assertIn("Lstr0@PAGEOFF" if target == "macos" else ":lo12:Lstr0", text)
                self.assertIn(".section __TEXT,__const" if target == "macos" else ".section .rodata", text)
                self.assertEqual("sub w0, w0, #256" in text, target == "macos")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_string_and_array_examples(self):
        self.execute((ROOT / "examples" / "strings.c").read_text(), b"Hello, Jetson!")
        self.execute((ROOT / "examples" / "arrays.c").read_text(), b"hello, arrays!\n")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_pointer_stores_aliasing_and_nested_calls(self):
        self.execute("""#include <stdio.h>
            int *identity(int *p){return p;}
            int bump(int *p){*p=*p+1;return *p;}
            int main(){int a=64;int b=0;int *p=&a;int **pp=&p;
            **pp=bump(&b)+64;putchar(*identity(p));
            *identity(p)=bump(&b)+64;putchar(a);
            int *q=0;if(q)return 1;q=&b;
            if(q==0)return 2;if(0!=q)return b-2;return 3;}
            """, b"AB")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_scaled_indexing_and_array_initializers(self):
        self.execute("""int total(int a[],int n){int i=0;int sum=0;
            while(i<n){sum=sum+a[i];i=i+1;}return sum;}
            int main(){int a[4]={10,20};a[2]=30;3[a]=40;
            int *p=a+3;if(p[-2]!=20)return 1;
            if(*(p-3)!=10)return 2;if(*(2+a)!=30)return 3;
            int *refs[3]={a,a+1,a+2};int **r=refs+2;
            **r=31;if(*r!=&a[2])return 4;
            if((*(&a+1))!=a+4)return 5;
            return total(a,4);}
            """, expected_status=101)
        self.execute("""#include <stdio.h>
            int main(){char a[5]={'A','B','C',0};a[1]='Z';
            putchar(a[0]);putchar(a[1]);putchar(a[2]);
            char b[2]="XY";putchar(b[1]);return a[3]+a[4];}
            """, b"AZCY")
        self.execute("int main(){int a;int *p=&a;*p=7;return a;}", expected_status=7)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_literals_and_char_conversions(self):
        source = r'''#include <stdio.h>
            int main(){char s[]="A\0B";putchar(s[0]);putchar(s[2]);
            char *p="C" "\x44\105\n";int i=0;while(p[i]){putchar(p[i]);i=i+1;}
            putchar('\t');putchar('\'');putchar('\\');return s[1];}'''
        self.execute(source, b"ABCDE\n\t'\\")
        expected = -1 if TARGET == "macos" else 255
        self.execute(f"""char narrow(int x){{return x;}}
            int main(){{char c=511;int a=(c=255);
            if(a!={expected})return 1;if(c!={expected})return 2;
            if(narrow(255)!={expected})return 3;return 0;}}""")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_mixed_pointer_char_native_abi(self):
        source = """#include <stdio.h>
            char *host(char*,int,char,int*,char**,int,char*,char);
            int generated(char *a,int b,char c,int *d,char **e,int f,char *g,char h){
                return *a+b+c+*d+**e+f+*g+h;}
            int main(){int n=7;char *a="AB";char *g="HI";
                char *p=host(a,2,3,&n,&a,4,g,5);putchar(*p);return 0;}
            """
        helper = """extern int generated(char*,int,char,int*,char**,int,char*,char);
            char *host(char *a,int b,char c,int *d,char **e,int f,char *g,char h){
                if(*a!=65 || b!=2 || c!=3 || *d!=7 || *e!=a || f!=4 || *g!=72 || h!=5)
                    return "X";
                if(generated(a,b,c,d,e,f,g,h)!=223)return "Y";
                return g+1;}
            """
        self.execute(source, b"I", helper=helper, helper_c=True)

    def test_typedef_void_globals_both_targets(self):
        source = (ROOT / "examples" / "globals.c").read_text()
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                symbol = "_total" if target == "macos" else "total"
                self.assertIn(f".globl {symbol}", text)
                self.assertIn(f"{symbol}:\n    .word 0", text)
                self.assertIn(f"adrp x0, {symbol}", text)
                self.assertIn(f"{symbol}@PAGEOFF" if target == "macos"
                              else f":lo12:{symbol}", text)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_typedef_void_globals_execution(self):
        self.execute((ROOT / "examples" / "globals.c").read_text(), b"A")
        self.execute("""typedef int Number; typedef Number Count;
            typedef char Text[4]; typedef int *Ref; typedef void Nothing;
            Count total; Count total; Count total=3; Count total;
            char byte=511; Ref ptr=0;
            Nothing set(Nothing); Nothing set(){ptr=&total;*ptr=*ptr+4;return;}
            int sum(Text s){return s[0]+s[1];}
            int main(){if(ptr!=0)return 1;set();
                Text a="AB";Text b="CD";if(sum(a)!=131)return 2;
                if(sum(b)!=135)return 3;
                {typedef char Number;typedef char Number;Number c=65;if(c!=65)return 4;}
                {int Number=9;Number=Number+1;if(Number!=10)return 5;}
                Number n=total;{int total=99;}if(n!=7)return 6;
                if(byte+1==0)return 0;if(byte==255)return 0;return 7;}
            """)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_global_and_void_native_abi(self):
        self.execute("""int count=40;char letter=65;int *ref;
            void host(void);void generated(int n){count=count+n;return;}
            int main(){ref=&count;host();if(*ref!=42)return 1;
                if(letter!=66)return 2;return 0;}
            """, helper="""extern int count;extern char letter;extern int *ref;
            extern void generated(int);
            void host(void){if(ref!=&count){count=99;return;}
                generated(2);letter=66;}
            """, helper_c=True)

    def test_invalid_typedef_void_globals(self):
        cases = [
            ("void f(){return 1;}int main(){}", "cannot return a value"),
            ("int main(){return;}", "must return a value"),
            ("void f(){}int main(){int n=f();}", "void expression"),
            ("void f(){}int main(){if(f())return 1;}", "void expression"),
            ("void f(){}int main(){while(f());}", "void expression"),
            ("void f(){}int main(){return f()+1;}", "void expression"),
            ("void f(){}int main(){return f()==f();}", "void expression"),
            ("void f(){}int g(int);int main(){g(f());}", "void expression"),
            ("void f(){}int main(){return f();}", "void expression"),
            ("void main(){}", "main must return int"),
            ("void x;int main(){}", "scalar type"),
            ("int main(){void x;}", "void type"),
            ("int main(){void x[2];}", "complete"),
            ("void f(void x);int main(){}", "parameter cannot"),
            ("typedef int T;char T;int main(){}", "conflicting global"),
            ("typedef int T;int f(int T,T x);int main(){}", "expected 'int'"),
            ("int x;char x;int main(){}", "conflicting global"),
            ("int x=1;int x=2;int main(){}", "duplicate global"),
            ("int x;int x(void);int main(){}", "already declared"),
            ("int x(void);int x;int main(){}", "already declared"),
            ("typedef int T;int main(){return T;}", "not an expression"),
            ("int main(){{typedef int T;}T x;}", "unknown local"),
            ("typedef char Text[];int main(){}", "explicit size"),
            ("int *p=1;int main(){}", "initializer must be zero"),
            ("int x=1+2;int main(){}", "expected ';'"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                result, _ = self.compile(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message.encode(), result.stderr)

    def test_invalid_pointer_and_array_operations(self):
        cases = [
            ("int main(){int *p=1;}", "incompatible pointer"),
            ("int main(){char *p=0;int *q=p;}", "incompatible pointer"),
            ("int main(){int a=1;return *a;}", "dereference requires"),
            ("int main(){int *p=&1;}", "address-of requires"),
            ("int main(){int a[2];a=0;}", "assignment requires"),
            ("int main(){int a=1;return a[0];}", "indexing requires"),
            ("int main(){int a[2];return a+a;}", "pointer arithmetic"),
            ("int main(){int a[2];return a-a;}", "pointer arithmetic"),
            ("int main(){int a[2];return a<a+1;}", "pointer ordering"),
            ("int main(){int a[2];return a==1;}", "pointer comparison"),
            ("int main(){int *p=0;return p;}", "pointer to integer"),
            ("int f(int*);int f(char*);int main(){}", "conflicting function"),
            ("char f(int);int f(int);int main(){}", "conflicting function"),
            ("int main(){int a[0];}", "array size"),
            ("int main(){int a[2000];}", "array size"),
            ("int main(){char a[];}", "explicit size"),
            ('int main(){char a[1]="ab";}', "too long"),
            ("int main(){int a[1]={1,2};}", "too many array"),
            (r'''int main(){char *p="\q";}''', "unsupported escape"),
            (r'''int main(){char *p="\x";}''', "hex escape"),
            (r'''int main(){char *p="\x100";}''', "fit a byte"),
            ("int main(){return 'ab';}", "one byte"),
            ('int main(){char *p="unterminated;}', "unterminated literal"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                result, _ = self.compile(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message.encode(), result.stderr)

    def test_enums_both_targets(self):
        source = """#include <stdio.h>
            enum Color { Red, Green = 5, Blue, Neg = -2 };
            typedef enum { Tick, Tock } Tick_Tock;
            enum Color pick(enum Color c);
            int main() {
                enum Color d = pick(Green);
                putchar('A' + d - Blue);
                Tick_Tock t = Tick;
                if (t != 0) return 1;
                putchar('0' + Tock);
                return 0;
            }
            enum Color pick(enum Color c) { if (c == Green) return Blue; return c; }
            """
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                prefix = "_" if target == "macos" else ""
                self.assertIn(f"bl {prefix}pick", text)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_enum_programs(self):
        self.execute("""enum Color { Red, Green = 5, Blue, Neg = -2 };
            enum Color pick(enum Color c);
            int main() {
                if (Red != 0) return 1;
                if (Green != 5) return 2;
                if (Blue != 6) return 3;
                if (Neg != -2) return 4;
                enum Color d = pick(Green);
                if (d != Blue) return 5;
                enum Color e = Blue + 1;
                if (e != 7) return 6;
                d = Neg;
                if (d >= 0) return 7;
                while (d < Neg) return 8;
                { enum Color { Red = 9 }; if (Red != 9) return 9; }
                if (Red != 0) return 10;
                typedef enum { One = 1 } Tick;
                Tick t = One;
                if (t != 1) return 11;
                if (pick(Blue) != Blue) return 12;
                return 0;
            }
            enum Color pick(enum Color c) { if (c == Green) return Blue; return c; }
            """)
        self.execute("""#include <stdio.h>
            enum Step { S0, S1, S2, S3 };
            int drive(enum Step s) {
                int n = 0;
                while (s != S3) { n = n + 1; s = s + 1; }
                return n;
            }
            int main() { putchar('0' + drive(S1)); return 0; }
            """, b"2")

    def test_invalid_enums(self):
        cases = [
            ("enum E x;int main(){}", "unknown enum tag"),
            ("enum E {A};enum E {B};int main(){}", "duplicate enum tag"),
            ("enum {A,A};int main(){}", "duplicate enumerator"),
            ("int main(){enum {A,A};return 0;}", "duplicate enumerator"),
            ("int main(){enum {A};enum {A};return 0;}", "duplicate enumerator"),
            ("enum {A=};int main(){}", "enumerator value"),
            ("enum {A=-};int main(){}", "enumerator value"),
            ("enum {5};int main(){}", "enumerator name"),
            ("enum;int main(){}", "enumerator list"),
            ("int main(){return E;}", "unknown local"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                result, _ = self.compile(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message.encode(), result.stderr)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_bool_programs(self):
        self.execute("""int main() {
                _Bool t = 5;
                _Bool f = 0;
                _Bool g = 0;
                if (t != 1) return 1;
                if (f != 0) return 2;
                t = 511;
                if (t != 1) return 3;
                t = 0;
                if (t != 0) return 4;
                char c = 300;
                _Bool b = c;
                if (b != 1) return 5;
                int n = 0;
                int *p = 0;
                _Bool pb = p;
                if (pb != 0) return 6;
                p = &n;
                pb = p;
                if (pb != 1) return 7;
                if (t) return 8;
                if (f) return 9;
                while (t) { t = 0; }
                if (t != f) return 10;
                return 0;
            }
            """)
        self.execute("""_Bool invert(_Bool b) { if (b) return 0; return 1; }
            int main() { _Bool a = invert(2); _Bool b = invert(0);
                if (a != 0) return 1; if (b != 1) return 2; return 0; }
            """)
        self.execute("""#include <stdio.h>
            _Bool host(_Bool b, _Bool *out);
            int generated(_Bool a, _Bool b) { return a + a + b; }
            int main() { _Bool t = 3; _Bool out = 0;
                _Bool r = host(t, &out);
                putchar('0' + r + out);
                putchar('0' + generated(1, 1));
                return 0; }
            """, b"13", helper="""#include <stdbool.h>
            _Bool host(_Bool b, _Bool *out) {
                if (b != true) return 0;
                *out = true;
                return false;
            }""", helper_c=True)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_multi_declarators(self):
        self.execute("""int main() {
                int a, b = 2, c[3];
                c[0] = a = b + 1;
                long x, y = 5, *p, arr[2];
                arr[0] = 1;
                arr[1] = 2;
                p = arr + 1;
                x = *p + y + c[0];
                if (a != 3) return 1;
                if (c[0] != 3) return 2;
                if (x != 10L) return 3;
                char n1 = 'A', n2 = 'B', text[2];
                text[0] = n2;
                if (n1 != 65) return 4;
                if (text[0] != 66) return 5;
                unsigned u1 = 1U, u2 = 2U, u3;
                u3 = u1 + u2;
                if (u3 != 3U) return 6;
                { int b = 9; if (b != 9) return 7; }
                if (b != 2) return 8;
                return 0;
            }
            """)
        self.execute("""typedef int Num, *NumRef, Row[4];
            Num total;
            NumRef ref;
            Num set(Num v) { total = v; ref = &total; return *ref; }
            int main() {
                Row row;
                row[2] = 9;
                if (set(row[2]) != 9) return 1;
                if (total != 9) return 2;
                Num a = 1, b = a + 1, c = b + 1;
                if (a + b + c != 6) return 3;
                return 0;
            }
            """)
        self.execute("""char g1 = 'A', g2 = 'B';
            long big1, big2 = 3000000000L;
            int main() { if (g1 != 65) return 1; if (g2 != 66) return 2;
                big1 = big2 + big2;
                if (big1 != 6000000000L) return 3; return 0; }
            """)

    def test_invalid_multi_declarators(self):
        cases = [
            ("int main(){int a, a;return 0;}", "duplicate local"),
            ("int main(){int a, *a;return 0;}", "duplicate local"),
            ("int main(){int a, char b;int main(){}", "expected an identifier"),
            ("int main(){int b=a, a=1;return b;}", "unknown local"),
            ("typedef int T;char T, U;int main(){}", "conflicting global"),
            ("int main(){typedef int A, *A;return 0;}", "duplicate local"),
            ("int main(){int x[2], y[];return 0;}", "explicit size"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                result, _ = self.compile(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message.encode(), result.stderr)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_long_unsigned_programs(self):
        self.execute("""int main() {
                long min = -9223372036854775807L - 1L;
                long max = 9223372036854775807L;
                if (min >= max) return 1;
                if (max <= 0L) return 2;
                if (min >= 0L) return 3;
                if (min + 1L != -9223372036854775807L) return 4;
                if (max - 1L != 9223372036854775806L) return 5;
                if (max + 0L != max) return 6;
                unsigned long umax = 18446744073709551615UL;
                if (0UL >= umax) return 7;
                if (umax + 1UL != 0UL) return 8;
                if (18446744073709551615UL <= 1UL) return 9;
                if (18446744073709551614UL >= 18446744073709551615UL) return 10;
                unsigned u = 2147483648U;
                if (u <= 1U) return 11;
                if (u >= 4294967295U) return 12;
                if (4294967295U <= 2147483648U) return 13;
                if (2147483648U + 1U != 2147483649U) return 14;
                long v = 2147483647;
                v = v + v + 1L;
                if (v != 4294967295L) return 15;
                if (v - 4294967296L != -1L) return 16;
                long w = 5000000000L;
                if (w - 4294967296L != 705032704L) return 17;
                if (w + (long)0 != w) return 18;
                long n = -5L;
                if (-n != 5L) return 19;
                if (-w != -5000000000L) return 20;
                unsigned z = 4294967295U;
                if (z + 1U != 0U) return 21;
                if (0L != 0L) return 22;
                return 0;
            }
            """)
        self.execute("""#include <stdio.h>
            long twice(long v);
            int main() {
                long v = 4000000000L;
                int ok = 0;
                if (twice(v) - 8000000000L == 0L) ok = 1;
                putchar('0' + ok);
                return 0;
            }
            long twice(long v) { return v + v; }
            """, b"1")
        self.execute("""long host(long v);
            unsigned long hostu(unsigned long v);
            int main() {
                if (host(3000000000L) != 6000000000L) return 1;
                if (hostu(18446744073709551615UL) != 18446744073709551614UL) return 2;
                return 0;
            }
            """, helper="""long host(long v) { return v + v; }
            unsigned long hostu(unsigned long v) { return v - 1UL; }
            """, helper_c=True)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_cast_programs(self):
        self.execute("""int main() {
                unsigned u = 2147483648U;
                if ((int)u != -2147483647 - 1) return 1;
                if ((long)u != 2147483648L) return 2;
                long l = (long)-1;
                if (l != -1L) return 3;
                if ((unsigned)l != 4294967295U) return 4;
                if ((char)300 != 44) return 5;
                if ((long)(int)4000000000L != 4000000000L - 4294967296L) return 6;
                if ((long)-5 != -5L) return 7;
                _Bool b = (_Bool)7;
                if (b != 1) return 8;
                if ((_Bool)0 != 0) return 9;
                int *p = (int *)0;
                if (p != 0) return 10;
                int n = 3;
                p = (int *)&n;
                if (*p != 3) return 11;
                if ((_Bool)p != 1) return 12;
                char c = 'A';
                if ((long)c != 65L) return 13;
                if ((int)c + 1 != 66) return 14;
                long w = (long)5L;
                if (w != 5L) return 15;
                if ((unsigned)3000000000L != 3000000000U) return 16;
                while ((long)0) return 17;
                if ((long)1) { } else return 18;
                return 0;
            }
            """)
        self.execute("int main(){long v = (long)2; if (v != 2L) return 1; return 0;}")
        self.execute("#include <stdio.h>\nint main(){putchar((char)'Q');return 0;}", b"Q")
        self.execute("int main(){return (int)3L;}", expected_status=3)

    def test_invalid_casts_and_wide_ops(self):
        cases = [
            ("int main(){return (int[3])0;}", "casts"),
            ("int main(){return (void)0;}", "convert to void"),
            ("int main(){int a;return (int a)0;}", "casts"),
            ("int main(){char *p;return (long)p;}", "pointer to integer"),
            ("int main(){int n;int *p=&n;return p;}", "pointer to integer"),
            ("int main(){long l;return l/2;}", "64-bit division"),
            ("int main(){unsigned u;return u%3;}", "unsupported character"),
            ("int main(){long l=9223372036854775808L;return 0;}", "too large for long"),
            ("int main(){unsigned u=4294967296U;return 0;}", "too large for unsigned"),
            ("int main(){return 1LL;}", "duplicate integer suffix"),
            ("int main(){return 1UU;}", "duplicate integer suffix"),
            ("int main(){return 1x;}", "decimal integer"),
            ("int main(){int *p=(char*)0;return 0;}", "incompatible pointer"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                result, _ = self.compile(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message.encode(), result.stderr)

    def test_struct_union_both_targets(self):
        source = """#include <stdio.h>
            enum E { E0, E1, E2 };
            struct Node { struct Node *next; enum E e; int n; char name[9]; };
            union Mixed { int whole; char bytes[4]; };
            typedef struct Rec { struct Rec *pc; enum E d;
                union { struct { enum E ec; int ic; char sc[5]; } v1;
                        struct { char pad[4]; char c2; } v2;
                        struct { char c1; char c2; } v3; } var; } Rec;
            Rec rec;
            struct Node list;
            int main() {
                if (sizeof(struct Node) != 32) return 1;
                if (sizeof(union Mixed) != 4) return 2;
                if (sizeof(Rec) != 32) return 3;
                rec.pc = 0;
                list.next = 0;
                if (list.next == 0) putchar('A'); else putchar('B');
                return 0;
            }
            """
        for target in ("linux", "macos"):
            with self.subTest(target=target):
                text = self.successful(source, target).read_text()
                self.assertIn(".zero 32", text)
                self.assertIn("adrp x0, _rec" if target == "macos"
                              else "adrp x0, rec", text)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_struct_member_operations(self):
        self.execute("""#include <stdio.h>
            enum E { E0, E1, E2 };
            struct Node { struct Node *next; enum E e; int n; char name[9]; };
            union Mixed { int whole; char bytes[4]; };
            typedef struct Rec { struct Rec *pc; enum E d;
                union { struct { enum E ec; int ic; char sc[5]; } v1;
                        struct { char pad[4]; char c2; } v2;
                        struct { char c1; char c2; } v3; } var; } Rec;
            Rec rec;
            struct Node list;
            struct Node *bump(struct Node *p) { p->n = p->n + 1; return p; }
            int main() {
                list.next = 0;
                list.e = E2;
                list.n = 7;
                if (list.e != 2) return 1;
                if (list.next != 0) return 2;
                struct Node *p = &list;
                if (p->n != 7) return 3;
                if (p->e != E2) return 4;
                p->next = p;
                if (p->next != p) return 5;
                if (bump(p)->n != 8) return 6;
                if (list.n != 8) return 7;
                list.name[0] = 'A';
                if (p->name[0] != 65) return 8;
                union Mixed m;
                m.whole = 1094861636;
                if (m.bytes[0] != 68) return 9;
                if (m.bytes[3] != 65) return 10;
                Rec r;
                r.pc = 0;
                r.d = E1;
                r.var.v1.ec = 3;
                r.var.v1.ic = 5;
                r.var.v1.sc[0] = 'A';
                if (r.var.v1.sc[0] != 65) return 11;
                r.var.v2.c2 = 90;
                if (r.var.v1.ic != 90) return 12;
                r.var.v3.c1 = 9;
                if (r.var.v1.ec != 9) return 13;
                if (r.d != E1) return 14;
                rec = r;
                if (rec.var.v1.ic != 90) return 15;
                if (rec.pc != 0) return 16;
                rec = rec;
                if (rec.var.v1.ec != 9) return 17;
                struct Node copy;
                copy = list;
                if (copy.next != p) return 18;
                if (copy.name[0] != 65) return 19;
                putchar(65);
                return 0;
            }
            """, b"A")
        self.execute("""struct Outer { int a; struct { char c; int n; } in; };
            int set(struct Outer *o, int v) { o->in.n = v; return o->in.n; }
            int main() {
                struct Outer o;
                o.a = 1;
                o.in.c = 2;
                o.in.n = 3;
                if (o.a + o.in.c + o.in.n != 6) return 1;
                if (set(&o, 9) != 9) return 2;
                if (o.in.n != 9) return 3;
                struct Outer *op = &o;
                if (op->a != 1) return 4;
                if ((*op).in.n != 9) return 5;
                if (sizeof(struct Outer) != 12) return 6;
                typedef struct Outer OuterT;
                if (sizeof(OuterT) != 12) return 7;
                return 0;
            }
            """)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_multidim_arrays(self):
        self.execute("""#include <stdio.h>
            int grid[4][5];
            typedef int Row5[5];
            typedef int Grid[4][5];
            int fill(Grid g, int rows, int cols) {
                int i = 0;
                while (i < rows) {
                    int j = 0;
                    while (j < cols) {
                        g[i][j] = i + j;
                        j = j + 1;
                    }
                    i = i + 1;
                }
                return 0;
            }
            int read(Grid g, int r, int c) { return g[r][c]; }
            int main() {
                if (sizeof(Grid) != 80) return 1;
                if (sizeof(Row5) != 20) return 2;
                fill(grid, 4, 5);
                if (grid[2][3] != 5) return 3;
                if (grid[0][0] != 0) return 4;
                if (grid[3][4] != 7) return 5;
                if (read(grid, 1, 2) != 3) return 6;
                Row5 *rp = grid + 2;
                if (rp[0][1] != 3) return 7;
                if (rp[1][0] != 3) return 8;
                if (rp[-1][3] != 4) return 9;
                Grid local;
                int i = 0;
                while (i < 4) {
                    int j = 0;
                    while (j < 5) { local[i][j] = i + j; j = j + 1; }
                    i = i + 1;
                }
                if (local[2][3] != 5) return 10;
                int sum = 0;
                i = 0;
                while (i < 4) {
                    int j = 0;
                    while (j < 5) { sum = sum + local[i][j]; j = j + 1; }
                    i = i + 1;
                }
                if (sum != 70) return 11;
                putchar(67);
                return 0;
            }
            """, b"C")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_global_aggregates(self):
        self.execute("""int ones[8];
            long counters[3];
            struct Pair { int a; int b; } pair;
            int main() {
                int i = 0;
                while (i < 8) {
                    if (ones[i] != 0) return 1;
                    ones[i] = i + 1;
                    i = i + 1;
                }
                if (ones[7] != 8) return 2;
                i = 0;
                while (i < 3) {
                    if (counters[i] != 0L) return 3;
                    counters[i] = 3000000000L + i;
                    i = i + 1;
                }
                if (counters[2] != 3000000002L) return 4;
                if (pair.a != 0) return 5;
                if (pair.b != 0) return 6;
                pair.a = 40;
                pair.b = 2;
                if (pair.a + pair.b != 42) return 7;
                struct Pair *pp = &pair;
                if (pp->a != 40) return 8;
                int arr[1000];
                arr[999] = 5;
                if (arr[999] != 5) return 9;
                return 0;
            }
            """)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_multiplication_division(self):
        self.execute("""int main() {
                if (5 * 6 != 30) return 1;
                if (100 / 7 != 14) return 2;
                if (-100 / 7 != -14) return 3;
                if (100 / -7 != -14) return 4;
                if (-100 / -7 != 14) return 5;
                if (-7 / 2 != -3) return 6;
                if (7 / -2 != -3) return 7;
                if ((-2147483647 - 1) / 2 != -1073741824) return 8;
                if (2147483647 * 2 != -2) return 9;
                if (-13 / 4 != -3) return 10;
                if (13 / -4 != -3) return 11;
                if (-13 / -4 != 3) return 12;
                if (2147483647 / 2147483647 != 1) return 13;
                unsigned z = 4294967295U;
                if (z / 65535U != 65537U) return 14;
                if (0 / 5 != 0) return 15;
                if (0 * 2147483647 != 0) return 16;
                if (2 + 3 * 4 - 6 / 2 != 11) return 17;
                if (7 * (3 - 1) - 2 / 2 != 13) return 18;
                unsigned u = 2147483648U;
                if (u * 2U != 0U) return 19;
                if ((2147483647 * 2147483647) != 1) return 20;
                int k = 0;
                int i = 0;
                for (k = 0; k < 100; k += 7) i = i + 1;
                if (i != 15) return 21;
                return 0;
            }
            """)
        self.execute("""int main() {
                long prod = 9223372036854775807L;
                prod = prod * 2L;
                if (prod != -2L) return 1;
                prod = -9223372036854775807L * 3L;
                if (prod != -9223372036854775805L) return 2;
                unsigned long u = 18446744073709551615UL;
                if (u * 3UL != 18446744073709551613UL) return 3;
                long v = 3000000000L;
                if (v * 2L != 6000000000L) return 4;
                if ((long)7 * 3L != 21L) return 5;
                if ((long)-5L * 1L != -5L) return 6;
                return 0;
            }
            """)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_logical_operators(self):
        self.execute("""#include <stdio.h>
            int calls = 0;
            int bump(void) { calls = calls + 1; return 1; }
            int zero(void) { calls = calls + 1; return 0; }
            int main() {
                if (!0 != 1) return 1;
                if (!5 != 0) return 2;
                if (!(3 && 4)) return 3;
                if (0 && 4) return 4;
                if (0 || 0) return 5;
                if (!(0 || 7)) return 6;
                if (!(12 | 10) != 0) return 7;
                if ((0 | 0) != 0) return 8;
                calls = 0;
                if (zero() && bump()) return 9;
                if (calls != 1) return 10;
                calls = 0;
                if (bump() && zero()) return 11;
                if (calls != 2) return 12;
                calls = 0;
                if (!(bump() || zero())) return 13;
                if (calls != 1) return 14;
                calls = 0;
                if (zero() || zero()) return 15;
                if (calls != 2) return 16;
                calls = 0;
                if (!(bump() && !zero())) return 17;
                if (calls != 2) return 18;
                putchar(70);
                return 0;
            }
            """, b"F")

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_compound_and_increment(self):
        self.execute("""int main() {
                int a = 5;
                a += 3;
                if (a != 8) return 1;
                a -= 10;
                if (a != -2) return 2;
                ++a;
                if (a != -1) return 3;
                --a; --a;
                if (a != -3) return 4;
                if (++a != -2) return 5;
                if (--a != -3) return 6;
                char c = 100;
                c += 100;
                if (c != 200) return 7;
                long big = 3000000000L;
                big += big;
                if (big != 6000000000L) return 8;
                int arr[4] = {1, 2, 0, 0};
                arr[2] += 5;
                if (arr[2] != 5) return 8;
                int i = 1;
                arr[i] -= 1;
                if (arr[1] != 1) return 9;
                ++arr[0];
                if (arr[0] != 2) return 10;
                --arr[3];
                if (arr[3] != -1) return 11;
                int *p = arr + 1;
                *p += 9;
                if (arr[1] != 10) return 12;
                ++*p;
                if (arr[1] != 11) return 12;
                int b = 0;
                b = ++a + 1;
                if (a != -2) return 13;
                if (b != -1) return 14;
                return 0;
            }
            """)

    @unittest.skipUnless(NATIVE, "requires a native AArch64 host")
    def test_native_for_do_switch(self):
        self.execute("""#include <stdio.h>
            enum Op { O1 = 1, O2 = 2, O3 = 3 };
            int main() {
                enum Op o = O2;
                int hit = 0;
                switch (o) {
                    case O1: hit = 1; break;
                    case O2: hit = 2; break;
                    case O3: hit = 3; break;
                }
                if (hit != 2) return 1;
                switch (o) {
                    case O1: hit = 11; break;
                    default: hit = 99; break;
                }
                if (hit != 99) return 2;
                int fall = 0;
                switch (o) {
                    case O2: fall = fall + 1;
                    case O3: fall = fall + 10; break;
                    default: fall = 100; break;
                }
                if (fall != 11) return 3;
                int brk = 0;
                int i = 0;
                for (i = 0; i < 10; ++i) {
                    if (i == 4) break;
                    brk = i;
                }
                if (brk != 3) return 4;
                while (1) { break; }
                switch (5) { default: break; }
                int nested = 0;
                for (i = 0; i < 3; ++i) {
                    int j = 0;
                    while (j < 3) {
                        if (j == 2) break;
                        ++j;
                    }
                    if (j == 2) { if (i == 2) break; }
                    ++nested;
                }
                if (nested != 2) return 5;
                int total = 0;
                i = 0;
                for (i = 0; i < 5; ++i) {
                    int j = 0;
                    do { total = total + 1; ++j; } while (j < i);
                }
                if (total != 11) return 6;
                int loops = 0;
                for (i = 10; i > 7; --i) ++loops;
                if (loops != 3) return 7;
                int empt = 0;
                for (i = 0; i < 0; ++i) ++loops;
                do ++loops; while (0);
                if (loops != 4) return 8;
                putchar(71);
                return 0;
            }
            """, b"G")

    def test_invalid_control_flow(self):
        cases = [
            ("int main(){continue;}", "continue"),
            ("int main(){while(1) continue;}", "continue"),
            ("int main(){break;}", "break outside"),
            ("int main(){case 1: return 0;}", "case label outside"),
            ("int main(){default: return 0;}", "default label outside"),
            ("int main(){switch(1){case 1:case 1:return 0;}}", "duplicate case value"),
            ("int main(){switch(1){case 1:break;default:break;default:break;}}", "duplicate default label"),
            ("int main(){switch(1){case 1+1:break;}}", "expected ':'"),
            ("int main(){switch(120){case 120:break;default:break;}}", None),
        ]
        for case in cases:
            source, message = case[0], case[1]
            with self.subTest(source=source):
                result, _ = self.compile(source)
                if message is None:
                    self.assertEqual(result.returncode, 0)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(message.encode(), result.stderr)

    def test_invalid_aggregates(self):
        cases = [
            ("struct S x;int main(){}", "unknown struct tag"),
            ("struct S {int a;};struct S {int b;};int main(){}", "duplicate struct tag"),
            ("struct S {int a;int a;};int main(){}", "duplicate member"),
            ("struct S {void v;};int main(){}", "member cannot have void type"),
            ("struct S {struct S s;};int main(){}", "member has incomplete type"),
            ("struct S {int a[];};int main(){}", "member has incomplete type"),
            ("struct S {};int main(){}", "at least one member"),
            ("struct S {int a;} s;int main(){s.x=1;return 0;}", "unknown member"),
            ("struct S {int a;} s;int main(){s=1;return 0;}", "incompatible aggregate"),
            ("struct S {int a;} s=5;int main(){}", "global aggregate initializers"),
            ("int a[2]={1,2};int main(){}", "global aggregate initializers"),
            ("struct S {int a;};struct S f(void);int main(){}", "aggregate results"),
            ("struct S {int a;};int f(struct S s);int main(){}", "aggregate parameters"),
            ("int main(){int *p;return p.a;}", "member access requires"),
            ("int main(){int **pp;return pp->a;}", "arrow access requires"),
            ("int main(){int a[2];return a.b;}", "member access requires"),
            ("int main(){int n;return sizeof(n);}", "sizeof requires"),
            ("int main(){int a[2][3];a[1]=5;return 0;}", "assignment requires"),
        ]
        for case in cases:
            source, message = case[0], case[1]
            with self.subTest(source=source):
                result, _ = self.compile(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message.encode(), result.stderr)

    @unittest.skipUnless(NATIVE and platform.system() == "Linux" and shutil.which("objdump"),
                         "requires native Linux AArch64 and objdump")
    def test_actual_machine_instruction_whitelist(self):
        asm = self.successful("""#include <stdio.h>
            typedef int Count; Count global;
            void set(void){global=1;return;}
            int sum(int n){if(n<=0)return 0;return n+sum(n-1);}
            int main(){set();int a=5;while(a<6){if(a!=0)a=a+(10+putchar(66));}char text[]="A";char *p=text;*p=65;putchar(*p);char *literal="Z";putchar(*literal);return sum(a-81);}
            """, "linux")
        obj = self.work / "output.o"
        cc = shlex.split(os.environ.get("TEST_CC", "cc"))
        result = run([*cc, "-c", asm, "-o", obj])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        # Default disassembly spells ADD x29, sp, #0 as the MOV alias.
        # Audit underlying instructions, not the disassembler's aliases.
        result = run(["objdump", "-d", "-M", "no-aliases", obj])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        mnemonics = re.findall(r"^\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+([a-z][a-z0-9]*)",
                               result.stdout.decode(), re.MULTILINE)
        self.assertIn("bl", mnemonics)
        self.assertIn("ret", mnemonics)
        self.assertIn("tbz", mnemonics)
        self.assertIn("ldrb", mnemonics)
        self.assertIn("strb", mnemonics)
        self.assertIn("adrp", mnemonics)
        self.assertLessEqual(set(mnemonics), ALLOWED)


if __name__ == "__main__":
    unittest.main(verbosity=2)
