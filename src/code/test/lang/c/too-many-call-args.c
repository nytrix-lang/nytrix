/*
 * C parser fixture exercising calls at and beyond the supported argument limit.
 */
long c_arg_limit_target(
    long arg0, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6,
    long arg7, long arg8, long arg9, long arg10, long arg11, long arg12,
    long arg13, long arg14, long arg15, long arg16, long arg17, long arg18,
    long arg19, long arg20, long arg21, long arg22, long arg23, long arg24,
    long arg25, long arg26, long arg27, long arg28, long arg29, long arg30,
    long arg31, long arg32, long arg33, long arg34, long arg35, long arg36,
    long arg37, long arg38, long arg39, long arg40, long arg41, long arg42,
    long arg43, long arg44, long arg45, long arg46, long arg47, long arg48,
    long arg49, long arg50, long arg51, long arg52, long arg53, long arg54,
    long arg55, long arg56, long arg57, long arg58, long arg59, long arg60,
    long arg61, long arg62, long arg63, long arg64);
long c_arg_limit_entry() {
  return c_arg_limit_target(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
