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
            ("int main(){return 1*2;}", "expected ';'"),
            ("int main(){1=2;}", "assignment requires a local"),
            ("int main(){int a=0; a+1=2;}", "assignment requires a local"),
            ("int main(){int a=0; +a=2;}", "assignment requires a local"),
            ("int main(){int a=0; (a=1)=2;}", "assignment requires a local"),
            ("int f(int); int main(){f(1)=2;}", "assignment requires a local"),
            ("int main(){missing=1;}", "unknown local"),
            ("int main(){int a=0; a=;}", "integer expression"),
            ("int main(){int a=0; a+=2;}", "integer expression"),
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
            ("int main(){while(1) break;}", "integer expression"),
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
            ("int x[2];int main(){}", "scalar type"),
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
