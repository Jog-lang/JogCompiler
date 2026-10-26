#ifndef JOG_COMMANDS_H
#define JOG_COMMANDS_H

/* One function per `jog` subcommand, each in its own cmd_*.c file so streams
   do not edit the same file. argv excludes "jog" and the command name.
   Return value is the process exit code. main.c only dispatches. */

int jog_cmd_lex(int argc, char **argv);    /* cmd_lex.c    lexer (done) */
int jog_cmd_parse(int argc, char **argv);  /* cmd_parse.c  parser stream */
int jog_cmd_check(int argc, char **argv);  /* cmd_build.c  driver stream */
int jog_cmd_emit_ir(int argc, char **argv); /* cmd_build.c driver stream */
int jog_cmd_emit_c(int argc, char **argv); /* cmd_build.c  driver stream */
int jog_cmd_build(int argc, char **argv);  /* cmd_build.c  driver stream */
int jog_cmd_run(int argc, char **argv);    /* cmd_build.c  driver stream */
int jog_cmd_test(int argc, char **argv);   /* cmd_test.c   driver stream, milestone 4 */

/* Prints "jog: '<name>' is not implemented yet (milestone N, ...)" and
   returns 2. For stub commands. */
int jog_cmd_not_implemented(const char *name, int milestone);

#endif
