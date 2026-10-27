"""Deterministic concurrent-stop checks, executed inside LLDB's Python runtime."""

from unittest.mock import Mock

import cpp_repl_lldb
import lldb


def verify():
    def context(description="step in", filename="__cpp_repl_dispatch"):
        thread = Mock(spec=lldb.SBThread)
        thread.GetStopReason.return_value = lldb.eStopReasonPlanComplete
        thread.GetStopDescription.return_value = description
        thread.GetThreadID.return_value = 1
        thread.GetProcess.return_value = [thread]
        ctx = Mock(spec=lldb.SBExecutionContext)
        ctx.GetThread.return_value = thread
        frame = ctx.GetFrame.return_value
        source = frame.GetLineEntry.return_value.GetFileSpec.return_value
        source.GetFilename.return_value = filename
        return ctx, thread

    hook = cpp_repl_lldb.DispatchStep(None, None, {})
    # A second thread's user stop takes precedence over completing our step.
    for name in dir(lldb):
        if not name.startswith("eStopReason"):
            continue
        reason = getattr(lldb, name)
        ctx, thread = context()
        other = Mock(spec=lldb.SBThread)
        other.GetThreadID.return_value = 2
        other.GetStopReason.return_value = reason
        thread.GetProcess.return_value.append(other)
        assert hook.handle_stop(ctx, None) is True
        if reason == lldb.eStopReasonNone:
            thread.StepInto.assert_called_once_with()
        else:
            thread.StepInto.assert_not_called()

    for description in ("step in", "step out"):
        ctx, thread = context(description)
        hook.handle_stop(ctx, None)
        thread.StepInto.assert_called_once_with()

    for description, filename in (
        ("instruction step into", "__cpp_repl_dispatch"),
        ("step in", "functions.cpp"),
    ):
        ctx, thread = context(description, filename)
        hook.handle_stop(ctx, None)
        thread.StepInto.assert_not_called()
    print("concurrent stop checks passed")
