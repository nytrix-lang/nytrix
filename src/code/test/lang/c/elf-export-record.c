int example_export(int value) { return value + 23; }

__asm__(".section \".export_symbol\",\"a\"\n"
        "__export_symbol_example_export:\n"
        ".asciz \"GPL\"\n"
        ".asciz \"\"\n"
        ".balign 8\n"
        ".quad example_export\n"
        ".previous\n");
