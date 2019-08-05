##############################################################
# Copyright 2019 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
##############################################################

from __future__ import print_function

import os
import sys
import logging
import argparse
import json

import flux
from flux import job
from flux.job import JobspecV1
from flux import util
from flux import constants


class CleanFormatter(argparse.HelpFormatter):
    def _format_action_invocation(self, action):
        if not action.option_strings:
            (metavar,) = self._metavar_formatter(action, action.dest)(1)
            return metavar

        else:
            opts = list(action.option_strings)

            #  Default optstring is `-l, --long-opt`
            optstring = ", ".join(opts)

            #  If only a long option is supported, then prefix with
            #   whitepsace by the width of a short option so that all
            #   long opts start in the same column:
            if len(opts) == 1 and len(opts[0]) > 2:
                optstring = "    " + opts[0]

            #  We're done if no argument supported
            if action.nargs == 0:
                return optstring

            #  Append option argument string after `=`
            default = action.dest.upper()
            args_string = self._format_args(action, default)
            return optstring + "=" + args_string


def make_fmt(formatter, argwidth=40):
    """
    Return our 'clean' HelpFormatter, if possible, with a wider default
     for the max width allowed for options.
    """
    try:
        # https://stackoverflow.com/a/5464440
        # beware: "Only the name of this class is considered a public API."
        # https://stackoverflow.com/questions/44333577
        return lambda prog: formatter(prog, max_help_position=argwidth)
    except TypeError:
        warnings.warn("argparse help formatter failed, falling back.")
        return formatter


class SubmitCmd:
    """
    SubmitCmd submits a job, displays the jobid on stdout, and returns.

    Usage: flux mini submit [OPTIONS] cmd ...
    """

    def __init__(self):
        self.parser = self.create_parser()

    def create_parser(self):
        """
        Create parser with args for submit subcommand
        """
        parser = argparse.ArgumentParser(add_help=False)
        parser.add_argument(
            "-N", "--nodes", type=int, metavar="N", help="Number of nodes to allocate"
        )
        parser.add_argument(
            "-n",
            "--ntasks",
            type=int,
            metavar="N",
            default=1,
            help="Number of tasks to start",
        )
        parser.add_argument(
            "-c",
            "--cores-per-task",
            type=int,
            metavar="N",
            default=1,
            help="Number of cores to allocate per task",
        )
        parser.add_argument(
            "-g",
            "--gpus-per-task",
            type=int,
            metavar="N",
            help="Number of GPUs to allocate per task",
        )
        parser.add_argument(
            "-t",
            "--time-limit",
            type=str,
            metavar="FSD",
            help="Time limit in Flux standard duration, e.g. 2d, 1.5h",
        )
        parser.add_argument(
            "--priority",
            help="Set job priority (0-31, default=16)",
            type=int,
            metavar="N",
            default=16,
        )
        parser.add_argument(
            "--job-name",
            type=str,
            help="Set an optional name for job to NAME",
            metavar="NAME",
        )
        parser.add_argument(
            "-o",
            "--setopt",
            action="append",
            help="Set shell option OPT. An optional value is supported with"
            + " OPT=VAL (default VAL=1) (multiple use OK)",
            metavar="OPT",
        )
        parser.add_argument(
            "--setattr",
            action="append",
            help="Set job attribute ATTR to VAL (multiple use OK)",
            metavar="ATTR=VAL",
        )
        parser.add_argument(
            "--input",
            type=str,
            help="Redirect job stdin from FILENAME, bypassing KVS",
            metavar="FILENAME",
        )
        parser.add_argument(
            "--output",
            type=str,
            help="Redirect job stdout to FILENAME, bypassing KVS",
            metavar="FILENAME",
        )
        parser.add_argument(
            "--error",
            type=str,
            help="Redirect job stderr to FILENAME, bypassing KVS",
            metavar="FILENAME",
        )
        parser.add_argument(
            "--label-io",
            action="store_true",
            help="Add rank labels to stdout, stderr lines",
        )
        parser.add_argument(
            "--flags",
            action="append",
            help="Set comma separated list of job submission flags. Possible "
            + "flags:  debug, waitable",
            metavar="FLAGS",
        )
        parser.add_argument(
            "--dry-run",
            action="store_true",
            help="Don't actually submit job, just emit jobspec",
        )
        parser.add_argument(
            "command", nargs=argparse.REMAINDER, help="Job command and arguments"
        )
        return parser

    def submit(self, args):
        """
        Submit job, constructing jobspec from args.
        Returns jobid.
        """
        if not args.command:
            raise ValueError("job command and arguments are missing")

        jobspec = JobspecV1.from_command(
            args.command,
            num_tasks=args.ntasks,
            cores_per_task=args.cores_per_task,
            gpus_per_task=args.gpus_per_task,
            num_nodes=args.nodes,
        )
        jobspec.cwd = os.getcwd()
        jobspec.environment = dict(os.environ)
        if args.time_limit is not None:
            jobspec.duration = args.time_limit

        if args.job_name is not None:
            jobspec.setattr("system.job.name", args.job_name)

        if args.input is not None:
            jobspec.setattr_shopt("input.stdin.type", "file")
            jobspec.setattr_shopt("input.stdin.path", args.input)

        if args.output is not None:
            jobspec.setattr_shopt("output.stdout.type", "file")
            jobspec.setattr_shopt("output.stdout.path", args.output)
            if args.label_io:
                jobspec.setattr_shopt("output.stdout.label", True)

        if args.error is not None:
            jobspec.setattr_shopt("output.stderr.type", "file")
            jobspec.setattr_shopt("output.stderr.path", args.error)
            if args.label_io:
                jobspec.setattr_shopt("output.stderr.label", True)

        if args.setopt is not None:
            for kv in args.setopt:
                # Split into key, val with a default for 1 if no val given:
                key, val = (kv.split("=", 1) + [1])[:2]
                try:
                    val = json.loads(val)
                except:
                    pass
                jobspec.setattr_shopt(key, val)

        if args.setattr is not None:
            for kv in args.setattr:
                tmp = kv.split("=", 1)
                if len(tmp) != 2:
                    raise ValueError("--setattr: Missing value for attr " + kv)
                key = tmp[0]
                try:
                    val = json.loads(tmp[1])
                except:
                    val = tmp[1]
                jobspec.setattr(key, val)

        flags = 0
        if args.flags is not None:
            for tmp in args.flags:
                for flag in tmp.split(","):
                    if flag == "debug":
                        flags |= flux.constants.FLUX_JOB_DEBUG
                    elif flag == "waitable":
                        flags |= flux.constants.FLUX_JOB_WAITABLE
                    else:
                        raise ValueError("--flags: Unknown flag " + flag)

        if args.dry_run:
            print(jobspec.dumps(), file=sys.stdout)
            sys.exit(0)

        h = flux.Flux()
        return job.submit(h, jobspec.dumps(), priority=args.priority, flags=flags)

    def main(self, args):
        jobid = self.submit(args)
        print(jobid, file=sys.stdout)

    def get_parser(self):
        return self.parser


class RunCmd(SubmitCmd):
    """
    RunCmd is identical to SubmitCmd, except it attaches the the job
    after submission.  Some additional options are added to modify the
    attach behavior.

    Usage: flux mini run [OPTIONS] cmd ...
    """

    def __init__(self):
        self.parser = self.create_parser()
        self.parser.add_argument(
            "-v",
            "--verbose",
            action="count",
            default=0,
            help="Increase verbosity on stderr (multiple use OK)",
        )

    def main(self, args):
        jobid = self.submit(args)

        # Display job id on stderr if -v
        # N.B. we must flush sys.stderr due to the fact that it is buffered
        # when it points to a file, and os.execvp leaves it unflushed
        if args.verbose > 0:
            print("jobid:", jobid, file=sys.stderr)
            sys.stderr.flush()

        # Build args for flux job attach
        attach_args = ["flux-job", "attach"]
        if args.label_io:
            attach_args.append("--label-io")
        if args.verbose > 1:
            attach_args.append("--show-events")
        if args.verbose > 2:
            attach_args.append("--show-exec")
        attach_args.append(str(jobid))

        # Exec flux-job attach, searching for it in FLUX_EXEC_PATH.
        os.environ["PATH"] = os.environ["FLUX_EXEC_PATH"] + ":" + os.environ["PATH"]
        os.execvp("flux-job", attach_args)


logger = logging.getLogger("flux-mini")


@util.CLIMain(logger)
def main():
    parser = argparse.ArgumentParser(prog="flux-mini")
    subparsers = parser.add_subparsers(
        title="supported subcommands", description="", dest="subcommand"
    )
    subparsers.required = True

    # run
    run = RunCmd()
    mini_run_parser_sub = subparsers.add_parser(
        "run",
        parents=[run.get_parser()],
        help="run a job interactively",
        formatter_class=make_fmt(CleanFormatter),
    )
    mini_run_parser_sub.set_defaults(func=run.main)

    # submit
    submit = SubmitCmd()
    mini_submit_parser_sub = subparsers.add_parser(
        "submit",
        parents=[submit.get_parser()],
        help="enqueue a job",
        formatter_class=make_fmt(CleanFormatter),
    )
    mini_submit_parser_sub.set_defaults(func=submit.main)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

# vi: ts=4 sw=4 expandtab
