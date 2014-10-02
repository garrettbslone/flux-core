/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <stdbool.h>
#include <sys/param.h>
#include <glob.h>
#include <flux/core.h>

#include "src/common/libutil/log.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/setenvf.h"
#include "src/common/libutil/argv.h"


void dump_environment (void);
void exec_subcommand (const char *searchpath, bool vopt, char *argv[]);
char *intree_config (void);

void setup_lua_env (zconfig_t *cf, const char *cpath_add, const char *path_add);
char *setup_exec_searchpath (zconfig_t *cf, const char *path_add);
void setup_module_env (zconfig_t *cf, const char *path_add);
void setup_cmbd_env (zconfig_t *cf, const char *path_override);

#define OPTIONS "+T:tx:hM:B:vc:L:C:"
static const struct option longopts[] = {
    {"tmpdir",          required_argument,  0, 'T'},
    {"trace-apisocket", no_argument,        0, 't'},
    {"exec-path",       required_argument,  0, 'x'},
    {"module-path",     required_argument,  0, 'M'},
    {"cmbd-path",       required_argument,  0, 'B'},
    {"lua-path",        required_argument,  0, 'L'},
    {"lua-cpath",       required_argument,  0, 'C'},
    {"config",          required_argument,  0, 'c'},
    {"verbose",         no_argument,        0, 'v'},
    {"help",            no_argument,        0, 'h'},
    {0, 0, 0, 0},
};

static void usage (void)
{
    fprintf (stderr, 
"Usage: flux [OPTIONS] COMMAND ARGS\n"
"    -x,--exec-path PATH      prepend PATH to command search path\n"
"    -M,--module-path PATH    prepend PATH to module search path\n"
"    -L,--lua-path PATH       prepend PATH to LUA_PATH\n"
"    -C,--lua-cpath PATH      prepend PATH to LUA_CPATH\n"
"    -T,--tmpdir PATH         set FLUX_TMPDIR\n"
"    -t,--trace-apisock       set FLUX_TRACE_APISOCK=1\n"
"    -B,--cmbd-path           override path to comms message broker\n"
"    -c,--config-file         set path to config file\n"
"    -v,--verbose             show FLUX_* environment and command search\n"
"\n"
"The flux-core commands are:\n"
"   keygen        Generate CURVE keypairs for session security\n"
"   start         Bootstrap a comms session interactively\n"
"   kvs           Access the Flux the key-value store\n"
"   module        Load/unload comms modules\n"
"   up            Show state of all broker ranks\n"
"   ping          Time round-trip RPC on the comms rank-request network\n"
"   mping         Time round-trip group RPC to the mecho comms module\n"
"   snoop         Snoop on local Flux message broker traffic\n"
"   event         Publish and subscribe to Flux events\n"
"   logger        Log a message to Flux logging system\n"
"   comms         Misc Flux comms session operations\n"
"   comms-stats   Display comms message counters, etc.\n"
"   topo          Display current comms topology using graphviz\n"
"   wreckrun      Execute a Flux lightweight job (LWJ)\n"
"   zio           Manipulate KVS streams (including LWJ stdio)\n"
);
}


int main (int argc, char *argv[])
{
    int ch;
    bool hopt = false;
    bool vopt = false;
    char *xopt = NULL;
    char *Mopt = NULL;
    char *Bopt = NULL;
    char *Lopt = NULL;
    char *Copt = NULL;
    char *config_file = NULL;
    zconfig_t *config;
    char *searchpath;

    log_init ("flux");

    while ((ch = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
        switch (ch) {
            case 'c': /* --config FILE */
                config_file = xstrdup (optarg);
                break;
            case 'T': /* --tmpdir PATH */
                if (setenv ("FLUX_TMPDIR", optarg, 1) < 0)
                    err_exit ("setenv FLUX_TMPDIR=%s", optarg);
                break;
            case 't': /* --trace-apisock */
                if (setenv ("FLUX_TRACE_APISOCK", "1", 1) < 0)
                    err_exit ("setenv FLUX_TRACE_APISOCK=1");
                break;
            case 'M': /* --module-path PATH */
                Mopt = optarg;
                break;
            case 'x': /* --exec-path PATH */
                xopt = optarg;
                break;
            case 'B': /* --cmbd-path PATH */
                Bopt = optarg;
                break;
            case 'v': /* --verbose */
                vopt = true;
                break;
            case 'L': /* --lua-path PATH */
                Lopt = optarg;
                break;
            case 'C': /* --lua-cpath PATH */
                Copt = optarg;
                break;
            case 'h': /* --help  */
                hopt = true;
                break;
            default:
                usage ();
                exit (1);
        }
    }
    argc -= optind;
    argv += optind;

    if (!config_file)
        config_file = intree_config ();
    config = flux_config_load (config_file, false);

    setup_lua_env (config, Lopt, Copt);
    setup_module_env (config, Mopt);
    setup_cmbd_env (config, Bopt);

    searchpath = setup_exec_searchpath (config, xopt);

    if (hopt) {
        if (argc > 0) {
            char *av[] = { argv[0], "--help", NULL };
            exec_subcommand (searchpath, vopt, av);
        } else
            usage ();
        exit (0);
    }
    if (argc == 0) {
        usage ();
        exit (1);
    }
    if (vopt) {
        dump_environment ();
        printf ("subcommand search path: %s", searchpath);
    }
    exec_subcommand (searchpath, vopt, argv);

    zconfig_destroy (&config);
    if (config_file)
        free (config_file);

    free (searchpath);
    log_fini ();

    return 0;
}

/*  Return directory containing this executable.  Caller must free.
 *   (using non-portable /proc/self/exe support for now)
 */
char *dir_self (void)
{
    char  flux_exe_path [MAXPATHLEN];
    char *flux_exe_dir;

    memset (flux_exe_path, 0, MAXPATHLEN);
    if (readlink ("/proc/self/exe", flux_exe_path, MAXPATHLEN - 1) < 0)
        err_exit ("readlink (/proc/self/exe)");
    flux_exe_dir = dirname (flux_exe_path);
    return xstrdup (flux_exe_dir);
}

char *intree_config (void)
{
    char *config_file = NULL;
    char *selfdir = dir_self ();

    if (strcmp (selfdir, X_BINDIR) != 0)
        config_file = xasprintf ("%s/../../flux.conf", selfdir);
    free (selfdir);
    return config_file;
}

static void path_push (char **path, const char *add, const char *sep)
{
    char *new = xasprintf ("%s%s%s", add, *path ? sep : "",
                                          *path ? *path : "");
    free (*path);
    *path = new;
}

void setup_lua_env (zconfig_t *cf, const char *cpath_add, const char *path_add)
{
    char *path = NULL, *cpath = NULL;;
    zconfig_t *z;

    path_push (&path, ";;", ";"); /* Lua replaces ;; with the default path */
    path_push (&cpath, ";;", ";");

    /* FIXME: push installed paths (needed if side-installed)
     */

    if ((z = zconfig_locate (cf, "general/lua_path"))) {
        char *val = zconfig_value (z);
        if (val && strlen (val) > 0)
            path_push (&path, val, ";");
    }
    if ((z = zconfig_locate (cf, "general/lua_cpath"))) {
        char *val = zconfig_value (z);
        if (val && strlen (val) > 0)
            path_push (&cpath, val, ";");
    }

    if (path_add)
        path_push (&path, path_add, ";");
    if (cpath_add)
        path_push (&cpath, cpath_add, ";");

    if (setenv ("LUA_CPATH", cpath, 1) < 0)
        err_exit ("%s", cpath);
    if (setenv ("LUA_PATH", path, 1) < 0)
        err_exit ("%s", path);

    free (path);
    free (cpath);
}

char *setup_exec_searchpath (zconfig_t *cf, const char *path_add)
{
    char *path = NULL;
    zconfig_t *z;

    path_push (&path, EXEC_PATH, ":");

    if ((z = zconfig_locate (cf, "general/exec_path"))) {
        char *val = zconfig_value (z);
        if (val && strlen (val) > 0)
            path_push (&path, val, ":");
    }

    if (path_add)
        path_push (&path, path_add, ":");

    return path;
}

void setup_module_env (zconfig_t *cf, const char *path_add)
{
    char *path = NULL;
    zconfig_t *z;

    path_push (&path, MODULE_PATH, ":");

    if ((z = zconfig_locate (cf, "general/module_path"))) {
        char *val = zconfig_value (z);
        if (val && strlen (val) > 0)
            path_push (&path, val, ":");
    }

    if (path_add)
        path_push (&path, path_add, ":");

    if (setenv ("FLUX_MODULE_PATH", path, 1) < 0)
        err_exit ("%s", path);

    free (path);
}

void setup_cmbd_env (zconfig_t *cf, const char *path_override)
{
    const char *path = NULL;
    zconfig_t *z;

    if (path_override)
        path = path_override;

    if (!path) {
        if ((z = zconfig_locate (cf, "general/cmbd_path"))) {
            char *val = zconfig_value (z);
            if (val && strlen (val) > 0)
                path = val;
        }
    }

    if (!path)
        path = CMBD_PATH;

    if (setenv ("FLUX_CMBD_PATH", path, 1) < 0)
        err_exit ("%s", path);
}

void dump_environment_one (const char *name)
{
    char *s = getenv (name);
    printf ("%20s%s%s\n", name, s ? "=" : "",
                                s ? s : " is not set");
}

void dump_environment (void)
{
    dump_environment_one ("FLUX_MODULE_PATH");
    dump_environment_one ("FLUX_CMBD_PATH");
    dump_environment_one ("FLUX_TMPDIR");
    dump_environment_one ("FLUX_TRACE_APISOCK");
    dump_environment_one ("LUA_PATH");
    dump_environment_one ("LUA_CPATH");
}

void exec_subcommand_dir (bool vopt, const char *dir,char *argv[],
                          const char *prefix)
{
    char *path;
    if (asprintf (&path, "%s/%s%s", dir, prefix ? prefix : "", argv[0]) < 0)
        oom ();
    if (vopt)
        msg ("trying to exec %s", path);
    execvp (path, argv); /* no return if successful */
    free (path);
}

void exec_subcommand (const char *searchpath, bool vopt, char *argv[])
{
    char *cpy = xstrdup (searchpath);
    char *dir, *saveptr = NULL, *a1 = cpy;

    while ((dir = strtok_r (a1, ":", &saveptr))) {
        exec_subcommand_dir (vopt, dir, argv, "flux-");
        //exec_subcommand_dir (vopt, dir, argv, NULL); /* deprecated */
        a1 = NULL;
    }
    free (cpy);
    msg_exit ("`%s' is not a flux command.  See 'flux --help'", argv[0]);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
