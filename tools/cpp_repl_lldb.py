"""Keep source-level step-in stops out of cpp-repl's generated dispatchers."""

import lldb


class DispatchStep:
    def __init__(self, target, extra_args, internal_dict):
        pass

    def handle_stop(self, exe_ctx, stream):
        thread = exe_ctx.GetThread()
        if thread.GetStopReason() != lldb.eStopReasonPlanComplete:
            return True
        # Preserve explicit instruction steps, breakpoints, watchpoints and
        # signals. Only source step-in/step-out stops should cross dispatchers.
        if thread.GetStopDescription(128) not in ("step in", "step out"):
            return True
        frame = exe_ctx.GetFrame()
        if frame.GetLineEntry().GetFileSpec().GetFilename() != "__cpp_repl_dispatch":
            return True
        # StepInto resumes the process, so another thread's stop must take
        # precedence even when this thread completed a step in a dispatcher.
        for other in thread.GetProcess():
            if (
                other.GetThreadID() != thread.GetThreadID()
                and other.GetStopReason() != lldb.eStopReasonNone
            ):
                return True
        thread.StepInto()
        return True


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("target stop-hook add -P cpp_repl_lldb.DispatchStep")
