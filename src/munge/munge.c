/*****************************************************************************
 *  $Id: munge.c,v 1.33 2004/08/21 05:50:49 dun Exp $
 *****************************************************************************
 *  This file is part of the Munge Uid 'N' Gid Emporium (MUNGE).
 *  For details, see <http://www.llnl.gov/linux/munge/>.
 *
 *  Copyright (C) 2003-2004 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Chris Dunlap <cdunlap@llnl.gov>.
 *  UCRL-CODE-155910.
 *
 *  This is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License;
 *  if not, write to the Free Software Foundation, Inc., 59 Temple Place,
 *  Suite 330, Boston, MA  02111-1307  USA.
 *****************************************************************************/


#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#include <sys/types.h>                  /* include before grp.h for bsd */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <munge.h>
#include "common.h"
#include "read.h"


/***************************************************************************** 
 *  Command-Line Options
 *****************************************************************************/

#include <getopt.h>
struct option opt_table[] = {
    { "help",         0, NULL, 'h' },
    { "license",      0, NULL, 'L' },
    { "version",      0, NULL, 'V' },
    { "no-input",     0, NULL, 'n' },
    { "string",       1, NULL, 's' },
    { "input",        1, NULL, 'i' },
    { "output",       1, NULL, 'o' },
    { "cipher",       1, NULL, 'c' },
    { "list-ciphers", 0, NULL, 'C' },
    { "mac",          1, NULL, 'm' },
    { "list-macs",    0, NULL, 'M' },
    { "zip",          1, NULL, 'z' },
    { "list-zips",    0, NULL, 'Z' },
    { "restrict-uid", 1, NULL, 'u' },
    { "restrict-gid", 1, NULL, 'g' },
    { "ttl",          1, NULL, 't' },
    { "socket",       1, NULL, 'S' },
    {  NULL,          0, NULL,  0  }
};

const char * const opt_string = "hLVns:i:o:c:Cm:Mz:Zu:g:t:S:";


/***************************************************************************** 
 *  Configuration
 *****************************************************************************/

struct conf {
    munge_ctx_t  ctx;                   /* munge context                     */
    munge_err_t  status;                /* error status munging the cred     */
    char        *string;                /* input from string instead of file */
    char        *fn_in;                 /* input filename, '-' for stdin     */
    char        *fn_out;                /* output filename, '-' for stdout   */
    FILE        *fp_in;                 /* input file pointer                */
    FILE        *fp_out;                /* output file pointer               */
    int          dlen;                  /* payload data length               */
    void        *data;                  /* payload data                      */
    int          clen;                  /* munged credential length          */
    char        *cred;                  /* munged credential nul-terminated  */
};

typedef struct conf * conf_t;


/***************************************************************************** 
 *  Prototypes
 *****************************************************************************/

conf_t create_conf (void);
void destroy_conf (conf_t conf);
void parse_cmdline (conf_t conf, int argc, char **argv);
void display_help (char *prog);
void display_strings (const char *header, const char **strings);
int str_to_int (const char *s, const char **strings);
void open_files (conf_t conf);
void display_cred (conf_t conf);


/***************************************************************************** 
 *  Functions
 *****************************************************************************/

int
main (int argc, char *argv[])
{
    conf_t       conf;
    int          rc = 0;
    const char  *p;

    if (posignal (SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_err (EMUNGE_SNAFU, LOG_ERR, "Unable to ignore signal=%d", SIGPIPE);
    }
    log_open_file (stderr, argv[0], LOG_INFO, LOG_OPT_PRIORITY);
    conf = create_conf ();
    parse_cmdline (conf, argc, argv);
    open_files (conf);

    if (conf->string) {
        rc = read_data_from_string (conf->string, &conf->data, &conf->dlen);
    }
    else if (conf->fn_in) {
        rc = read_data_from_file (conf->fp_in, &conf->data, &conf->dlen);
    }
    if (rc < 0) {
        if (errno == ENOMEM) {
            log_errno (EMUNGE_NO_MEMORY, LOG_ERR, "Unable to read input");
        }
        else {
            log_err (EMUNGE_SNAFU, LOG_ERR, "Read error");
        }
    }
    conf->status = munge_encode (&conf->cred, conf->ctx,
        conf->data, conf->dlen);

    if (conf->status != EMUNGE_SUCCESS) {
        if (!(p = munge_ctx_strerror (conf->ctx))) {
            p = munge_strerror (conf->status);
        }
        log_err (conf->status, LOG_ERR, "%s", p);
    }
    conf->clen = strlen (conf->cred);

    display_cred (conf);

    destroy_conf (conf);
    exit (EMUNGE_SUCCESS);
}


conf_t
create_conf (void)
{
    conf_t conf;

    if (!(conf = malloc (sizeof (struct conf)))) {
        log_errno (EMUNGE_NO_MEMORY, LOG_ERR, "Unable to create conf");
    }
    if (!(conf->ctx = munge_ctx_create())) {
        log_errno (EMUNGE_NO_MEMORY, LOG_ERR, "Unable to create conf ctx");
    }
    conf->status = -1;
    conf->string = NULL;
    conf->fn_in = "-";
    conf->fn_out = "-";
    conf->fp_in = NULL;
    conf->fp_out = NULL;
    conf->dlen = 0;
    conf->data = NULL;
    conf->clen = 0;
    conf->cred = NULL;
    return (conf);
}


void
destroy_conf (conf_t conf)
{
    /*  XXX: Don't free conf's string/fn_in/fn_out
     *       since they point inside argv[].
     */
    if (conf->ctx != NULL) {
        munge_ctx_destroy (conf->ctx);
    }
    if (conf->fp_in != NULL) {
        if (fclose (conf->fp_in) < 0) {
            log_errno (EMUNGE_SNAFU, LOG_ERR, "Unable to close infile");
        }
        conf->fp_in = NULL;
    }
    if (conf->fp_out != NULL) {
        if ((fclose (conf->fp_out) < 0) && (errno != EPIPE)) {
            log_errno (EMUNGE_SNAFU, LOG_ERR, "Unable to close outfile");
        }
        conf->fp_out = NULL;
    }
    if (conf->data != NULL) {
        memburn (conf->data, 0, conf->dlen);
        free (conf->data);
        conf->data = NULL;
    }
    if (conf->cred != NULL) {
        memburn (conf->cred, 0, conf->clen);
        free (conf->cred);
        conf->cred = NULL;
    }
    free (conf);
    return;
}


void
parse_cmdline (conf_t conf, int argc, char **argv)
{
    char           *prog;
    int             c;
    char           *p;
    munge_err_t     e;
    int             i = 0;              /* suppress false compiler warning */
    struct passwd  *pw_ptr;
    struct group   *gr_ptr;

    opterr = 0;                         /* suppress default getopt err msgs */

    prog = (prog = strrchr (argv[0], '/')) ? prog + 1 : argv[0];

    for (;;) {

        c = getopt_long (argc, argv, opt_string, opt_table, NULL);

        if (c == -1) {                  /* reached end of option list */
            break;
        }
        switch (c) {
            case 'h':
                display_help (prog);
                exit (EMUNGE_SUCCESS);
                break;
            case 'L':
                display_license ();
                exit (EMUNGE_SUCCESS);
                break;
            case 'V':
                printf ("%s-%s\n", PACKAGE, VERSION);
                exit (EMUNGE_SUCCESS);
                break;
            case 'n':
                conf->fn_in = NULL;
                conf->string = NULL;
                break;
            case 's':
                conf->fn_in = NULL;
                conf->string = optarg;
                break;
            case 'i':
                conf->fn_in = optarg;
                conf->string = NULL;
                break;
            case 'o':
                conf->fn_out = optarg;
                break;
            case 'c':
                if ((i = str_to_int (optarg, munge_cipher_strings)) < 0) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Invalid cipher type \"%s\"", optarg);
                }
                e = munge_ctx_set (conf->ctx, MUNGE_OPT_CIPHER_TYPE, i);
                if (e != EMUNGE_SUCCESS) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unable to set cipher type: %s",
                        munge_ctx_strerror (conf->ctx));
                }
                break;
            case 'C':
                display_strings ("Cipher types", munge_cipher_strings);
                exit (EMUNGE_SUCCESS);
                break;
            case 'm':
                if ((i = str_to_int (optarg, munge_mac_strings)) < 0) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Invalid message auth code type \"%s\"", optarg);
                }
                e = munge_ctx_set (conf->ctx, MUNGE_OPT_MAC_TYPE, i);
                if (e != EMUNGE_SUCCESS) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unable to set message auth code type: %s",
                        munge_ctx_strerror (conf->ctx));
                }
                break;
            case 'M':
                display_strings ("MAC types", munge_mac_strings);
                exit (EMUNGE_SUCCESS);
                break;
            case 'z':
                if ((i = str_to_int (optarg, munge_zip_strings)) < 0) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Invalid compression type \"%s\"", optarg);
                }
                e = munge_ctx_set (conf->ctx, MUNGE_OPT_ZIP_TYPE, i);
                if (e != EMUNGE_SUCCESS) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unable to set compression type: %s",
                        munge_ctx_strerror (conf->ctx));
                }
                break;
            case 'Z':
                display_strings ("Compression types", munge_zip_strings);
                exit (EMUNGE_SUCCESS);
                break;
            case 'u':
                if (strlen (optarg) == 0) {
                    i = getuid ();
                }
                else if (strlen (optarg) == strspn (optarg, "0123456789")) {
                    i = strtol (optarg, NULL, 10);
                }
                else if ((pw_ptr = getpwnam (optarg)) != NULL) {
                    i = pw_ptr->pw_uid;
                }
                else {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unrecognized user \"%s\"", optarg);
                }
                e = munge_ctx_set (conf->ctx, MUNGE_OPT_UID_RESTRICTION, i);
                if (e != EMUNGE_SUCCESS) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unable to set uid restriction: %s",
                        munge_ctx_strerror (conf->ctx));
                }
                break;
            case 'g':
                if (strlen (optarg) == 0) {
                    i = getgid ();
                }
                else if (strlen (optarg) == strspn (optarg, "0123456789")) {
                    i = strtol (optarg, NULL, 10);
                }
                else if ((gr_ptr = getgrnam (optarg)) != NULL) {
                    i = gr_ptr->gr_gid;
                }
                else {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unrecognized group \"%s\"", optarg);
                }
                e = munge_ctx_set (conf->ctx, MUNGE_OPT_GID_RESTRICTION, i);
                if (e != EMUNGE_SUCCESS) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unable to set gid restriction: %s",
                        munge_ctx_strerror (conf->ctx));
                }
                break;
            case 't':
                i = strtol (optarg, &p, 10);
                if ((optarg == p) || (*p != '\0')) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Invalid time-to-live '%s'", optarg);
                }
                if ((i == LONG_MAX) && (errno == ERANGE)) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Exceeded maximum time-to-live of %d seconds",
                        LONG_MAX);
                }
                if (i < 0) {
                    i = MUNGE_TTL_MAXIMUM;
                }
                e = munge_ctx_set (conf->ctx, MUNGE_OPT_TTL, i);
                if (e != EMUNGE_SUCCESS) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unable to set time-to-live: %s",
                        munge_ctx_strerror (conf->ctx));
                }
                break;
            case 'S':
                e = munge_ctx_set (conf->ctx, MUNGE_OPT_SOCKET, optarg);
                if (e != EMUNGE_SUCCESS) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Unable to set munge socket name: %s",
                        munge_ctx_strerror (conf->ctx));
                }
                break;
            case '?':
                if (optopt > 0) {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Invalid option \"-%c\"", optopt);
                }
                else {
                    log_err (EMUNGE_SNAFU, LOG_ERR,
                        "Invalid option \"%s\"", argv[optind - 1]);
                }
                break;
            default:
                log_err (EMUNGE_SNAFU, LOG_ERR,
                    "Unimplemented option \"%s\"", argv[optind - 1]);
                break;
        }
    }
    if (argv[optind]) {
        log_err (EMUNGE_SNAFU, LOG_ERR,
            "Unrecognized parameter \"%s\"", argv[optind]);
    }
    return;
}


void
display_help (char *prog)
{
/*  Displays a help message describing the command-line options.
 */
    const int w = -25;                  /* pad for width of option string */

    assert (prog != NULL);

    printf ("Usage: %s [OPTIONS]\n", prog);
    printf ("\n");

    printf ("  %*s %s\n", w, "-h, --help",
            "Display this help");

    printf ("  %*s %s\n", w, "-L, --license",
            "Display license information");

    printf ("  %*s %s\n", w, "-V, --version",
            "Display version information");

    printf ("\n");

    printf ("  %*s %s\n", w, "-n, --no-input",
            "Redirect input from /dev/null");

    printf ("  %*s %s\n", w, "-s, --string=STRING",
            "Input payload data from STRING");

    printf ("  %*s %s\n", w, "-i, --input=FILE",
            "Input payload data from FILE");

    printf ("  %*s %s\n", w, "-o, --output=FILE",
            "Output credential to FILE");

    printf ("\n");

    printf ("  %*s %s\n", w, "-c, --cipher=STRING",
            "Specify cipher type");

    printf ("  %*s %s\n", w, "-C, --list-ciphers",
            "Print a list of supported ciphers");

    printf ("  %*s %s\n", w, "-m, --mac=STRING",
            "Specify message authentication code type");

    printf ("  %*s %s\n", w, "-M, --list-macs",
            "Print a list of supported MACs");

    printf ("  %*s %s\n", w, "-z, --zip=STRING",
            "Specify compression type");

    printf ("  %*s %s\n", w, "-Z, --list-zips",
            "Print a list of supported compressions");

    printf ("\n");

    printf ("  %*s %s\n", w, "-u, --restrict-uid=UID",
            "Restrict credential decoding to only this UID");

    printf ("  %*s %s\n", w, "-g, --restrict-gid=GID",
            "Restrict credential decoding to only this GID");

    printf ("  %*s %s\n", w, "-t, --ttl=INTEGER",
            "Specify time-to-live (in seconds; 0=default -1=max)");

    printf ("  %*s %s\n", w, "-S, --socket=STRING",
            "Specify local domain socket");

    printf ("\n");
    printf ("By default, data is read from stdin and written to stdout.\n\n");
    return;
}


void
display_strings (const char *header, const char **strings)
{
    const char **pp;
    int i;

    /*  Display each non-empty string in the NULL-terminated list.
     *    Empty strings (ie, "") are invalid.
     */
    printf ("%s:\n\n", header);
    for (pp=strings, i=0; *pp; pp++, i++) {
        if (*pp[0] != '\0') {
            printf ("  %s (%d)\n", *pp, i);
        }
    }
    printf ("\n");
    return;
}


int
str_to_int (const char *s, const char **strings)
{
    const char **pp;
    char *p;
    int i;
    int n;

    if (!s || !*s) {
        return (-1);
    }
    /*  Check to see if the given string matches a valid string.
     *  Also determine the number of strings in the array.
     */
    for (pp=strings, i=0; *pp; pp++, i++) {
        if (!strcasecmp (s, *pp)) {
            return (i);
        }
    }
    /*  Check to see if the given string matches a valid enum.
     */
    n = strtol (s, &p, 10);
    if ((s == p) || (*p != '\0')) {
        return (-1);
    }
    if ((n < 0) || (n >= i)) {
        return (-1);
    }
    if (strings[n][0] == '\0') {
        return (-1);
    }
    return (n);
}


void
open_files (conf_t conf)
{
    if (conf->fn_in) {
        if (!strcmp (conf->fn_in, "-")) {
            conf->fp_in = stdin;
        }
        else if (!(conf->fp_in = fopen (conf->fn_in, "r"))) {
            log_errno (EMUNGE_SNAFU, LOG_ERR,
                "Unable to read from \"%s\"", conf->fn_in);
        }
    }
    if (conf->fn_out) {
        if (!strcmp (conf->fn_out, "-")) {
            conf->fp_out = stdout;
        }
        else if (!(conf->fp_out = fopen (conf->fn_out, "w"))) {
            log_errno (EMUNGE_SNAFU, LOG_ERR,
                "Unable to write to \"%s\"", conf->fn_out);
        }
    }
    return;
}


void
display_cred (conf_t conf)
{
    if (!conf->fp_out) {
        return;
    }
    if (fprintf (conf->fp_out, "%s\n", conf->cred) < 0) {
        log_errno (EMUNGE_SNAFU, LOG_ERR, "Write error");
    }
    return;
}
