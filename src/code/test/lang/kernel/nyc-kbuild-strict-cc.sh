#!/usr/bin/env bash
set -e
NYC_BIN=${NYC_BIN:?NYC_BIN must name the tested compiler}
is_c=0
compile=0
language=
next_language=0
for argument in "$@"; do
  if [ "$next_language" = 1 ]; then
    language=$argument
    next_language=0
    continue
  fi
  case "$argument" in
    -x) next_language=1 ;;
    -xc) language=c ;;
    -xcpp-output) language=cpp-output ;;
    -xassembler*) language=assembler ;;
    *.c|*.i|-) is_c=1 ;;
    -c) compile=1 ;;
    --version) exec "$NYC_BIN" "$@" ;;
  esac
done
case "$language" in
  c|cpp-output) is_c=1 ;;
  assembler|assembler-with-cpp) is_c=0 ;;
esac
if [ "$is_c" = 0 ]; then
  # MS anonymous-structure syntax is a C-only NYC option. GNU's assembly
  # preprocessor has no C declarations to interpret and rejects this spelling.
  assembler_arguments=()
  for argument in "$@"; do
    [ "$argument" = -fms-anonymous-structs ] ||
      assembler_arguments+=("$argument")
  done
  exec gcc "${assembler_arguments[@]}"
fi
if [ "$compile" = 0 ]; then
  exec "$NYC_BIN" "$@"
fi
arguments=("$@")
output=
assembly_arguments=()
for ((i=0; i<${#arguments[@]}; ++i)); do
  argument=${arguments[$i]}
  if [ "$argument" = -c ]; then
    assembly_arguments+=(-S)
  elif [ "$argument" = -o ]; then
    ((i+=1))
    output=${arguments[$i]}
    assembly_arguments+=(-o "${output%.o}.nyc.s")
  else
    assembly_arguments+=("$argument")
  fi
done
[ -n "$output" ]
assembly="${output%.o}.nyc.s"
"$NYC_BIN" "${assembly_arguments[@]}"
assembler_mode=--64
for argument in "$@"; do
  case "$argument" in -m32|-m16) assembler_mode=--32 ;; esac
done
as "$assembler_mode" -o "$output" "$assembly"
rm -f "$assembly"
