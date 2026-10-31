// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;
using LibOSDP.Interop;

namespace LibOSDP;

/// <summary>
/// An OSDP peripheral device answering a control panel on one address. Call
/// <see cref="RunAsync"/> (or <see cref="Refresh"/> at least every 50 ms),
/// and always <see cref="Dispose"/> it: LibOSDP holds a handle to this
/// object, so it is never garbage collected otherwise.
/// </summary>
public sealed unsafe class PeripheralDevice : IDisposable
{
    private readonly object _gate = new();
    private readonly bool _forwardNotifications;
    private readonly LinkState _link = new(1);
    private readonly HandlerErrors _errors = new();
    private readonly PendingSubmissions _pending = new();
    private GCHandle _fileOps;
    private GCHandle _self;
    private GCHandle _channel;
    private nint _ctx;
    private int _disposed;

    /// <summary>
    /// Decides how each command is answered; null acknowledges everything.
    /// Runs on the refresh thread while the bus is locked. It may call
    /// <see cref="SubmitEventAsync"/>: an event submitted from here is sent as
    /// the reply to this command (how osdp_MFG, BIOREAD and BIOMATCH get
    /// inline answers). It must not call <see cref="Refresh"/> or
    /// <see cref="Dispose"/>; both throw <see cref="InvalidOperationException"/>
    /// there, which then surfaces from <see cref="Refresh"/>.
    /// </summary>
    public CommandHandler? CommandHandler { get; set; }

    /// <summary>Set up a peripheral device.</summary>
    /// <param name="channel">The link to the control panel.</param>
    /// <param name="info">This PD's address, key, identity and flags.</param>
    /// <param name="capabilities">What this PD advertises; LibOSDP refuses commands outside it.</param>
    public PeripheralDevice(OsdpChannel channel, PdInfo info, IReadOnlyList<PdCapability> capabilities)
    {
        ArgumentNullException.ThrowIfNull(channel);
        ArgumentNullException.ThrowIfNull(info);
        ArgumentNullException.ThrowIfNull(capabilities);
        Osdp.EnsureInitialized();

        _forwardNotifications = info.Flags.HasFlag(OsdpFlags.EnableNotification);
        _self = GCHandle.Alloc(this);
        _channel = GCHandle.Alloc(new ChannelHost(channel, _errors));
        NativeChannel native = ChannelBridge.Describe(_channel);
        try
        {
            using var infos = new NativePdInfoList([info], OsdpFlags.EnableNotification, capabilities);
            _ctx = Native.PdSetup(&native, infos.Pointer);
            if (_ctx == 0)
                throw new OsdpException("osdp_pd_setup failed; check the PD info and capabilities.");
        }
        catch
        {
            _self.Free();
            _channel.Free();
            throw;
        }
        nint self = GCHandle.ToIntPtr(_self);
        // Nothing has refreshed the new context yet, so neither is refused.
        _ = Native.PdSetCommandCallback(_ctx, &OnCommand, self);
        _ = Native.PdSetEventCompletionCallback(_ctx, &OnEventCompleted, self);
    }

    /// <summary>Whether a control panel is talking to this PD.</summary>
    public bool IsOnline => _link.IsOnline(0);

    /// <summary>Whether the secure channel is active.</summary>
    public bool IsSecureChannelActive => _link.IsSecure(0);

    /// <summary>Service the link once. Rethrows anything a handler or the channel threw.</summary>
    /// <exception cref="InvalidOperationException">Called from inside <see cref="CommandHandler"/>.</exception>
    public void Refresh()
    {
        ThrowIfInsideCallback(nameof(Refresh));
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            Native.PdRefresh(_ctx);
        }
        _errors.ThrowIfAny();
    }

    /// <summary>
    /// Service the link every <see cref="Osdp.RefreshInterval"/> until cancelled
    /// (throws <see cref="OperationCanceledException"/>) or disposed (returns).
    /// </summary>
    /// <param name="cancellationToken">Stops the loop.</param>
    /// <returns>A task that faults if a handler or the channel threw.</returns>
    public Task RunAsync(CancellationToken cancellationToken = default) =>
        RefreshLoop.RunAsync(Refresh, () => Volatile.Read(ref _disposed) != 0, cancellationToken);

    /// <summary>
    /// Queue an event for the control panel. The returned task completes when
    /// LibOSDP is done with it: sent, failed, flushed, or aborted by <see cref="Dispose"/>.
    /// </summary>
    /// <param name="ev">The event.</param>
    /// <returns>How the event settled.</returns>
    /// <exception cref="ArgumentException">Invalid event.</exception>
    /// <exception cref="OsdpException">LibOSDP refused it (e.g. a status report larger than the advertised capability).</exception>
    public Task<CompletionStatus> SubmitEventAsync(OsdpEvent ev)
    {
        ArgumentNullException.ThrowIfNull(ev);
        var native = (NativeEvent*)NativeHeap.AllocZeroed((nuint)sizeof(NativeEvent));
        try
        {
            EventMarshal.ToNative(ev, native);
        }
        catch
        {
            NativeHeap.Free(native);
            throw;
        }
        Task<CompletionStatus> completion = _pending.Track(native, sizeof(NativeEvent));
        int rc;
        lock (_gate)
            rc = _ctx == 0 ? int.MinValue : Native.PdSubmitEvent(_ctx, native);
        if (rc == 0)
            return completion;
        _pending.Discard(native);
        ObjectDisposedException.ThrowIf(rc == int.MinValue, this);
        throw new OsdpException($"LibOSDP refused {ev.GetType().Name}.");
    }

    /// <summary>Wait until a control panel is talking to this PD.</summary>
    /// <param name="timeout">How long to wait.</param>
    /// <param name="cancellationToken">Stops waiting.</param>
    /// <returns>False on timeout.</returns>
    public Task<bool> WaitOnlineAsync(TimeSpan timeout, CancellationToken cancellationToken = default) =>
        _link.WaitAsync(() => _link.IsOnline(0), timeout, cancellationToken);

    /// <summary>Wait until the secure channel is active.</summary>
    /// <param name="timeout">How long to wait.</param>
    /// <param name="cancellationToken">Stops waiting.</param>
    /// <returns>False on timeout.</returns>
    public Task<bool> WaitSecureChannelAsync(TimeSpan timeout, CancellationToken cancellationToken = default) =>
        _link.WaitAsync(() => _link.IsSecure(0), timeout, cancellationToken);

    /// <summary>Drop queued events; each completes as <see cref="CompletionStatus.Flushed"/>.</summary>
    /// <returns>How many were dropped.</returns>
    public int FlushEvents()
    {
        int n;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            n = Native.PdFlushEvents(_ctx);
        }
        return Math.Max(n, 0);
    }

    /// <summary>
    /// Longest R-APDU a <see cref="TrsCardDataEvent"/> can carry to the CP. It
    /// changes when the CP announces its receive size, so ask for each answer;
    /// a longer one is refused, and should be answered with a
    /// <see cref="TrsErrorEvent"/> instead.
    /// </summary>
    /// <returns>The length in bytes.</returns>
    public int GetTrsMaxApduLength()
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            return Native.PdTrsGetMaxApduLen(_ctx);
        }
    }

    /// <summary>Provide storage for files the CP sends.</summary>
    /// <param name="operations">The file sink.</param>
    /// <exception cref="InvalidOperationException">The link has already been refreshed.</exception>
    /// <remarks>Call before the first <see cref="Refresh"/> or <see cref="RunAsync"/>.</remarks>
    public void RegisterFileOperations(IFileOperations operations)
    {
        ArgumentNullException.ThrowIfNull(operations);
        GCHandle host = GCHandle.Alloc(new FileOpsHost(operations, _errors));
        NativeFileOps ops = FileOpsBridge.Describe(host);
        int rc;
        lock (_gate)
        {
            if (_ctx == 0)
            {
                host.Free();
                throw new ObjectDisposedException(nameof(PeripheralDevice));
            }
            rc = Native.FileRegisterOps(_ctx, 0, &ops);
            if (rc == 0)
            {
                // LibOSDP copied the ops and dropped the old arg.
                if (_fileOps.IsAllocated)
                    _fileOps.Free();
                _fileOps = host;
            }
        }
        if (rc != 0)
        {
            host.Free();
            throw new InvalidOperationException("File operations must be registered before the first refresh.");
        }
    }

    /// <summary>Progress of a running file transfer, or null when none is running.</summary>
    /// <returns>The progress.</returns>
    public FileTransferStatus? GetFileTransferStatus()
    {
        uint size, offset;
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = Native.GetFileTxStatus(_ctx, 0, &size, &offset);
        }
        return rc == 0 ? new FileTransferStatus(size, offset) : null;
    }

    /// <summary>Take the link counters; this resets them.</summary>
    /// <returns>The counters.</returns>
    public Metrics GetMetrics()
    {
        NativeMetrics m;
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = Native.GetMetrics(_ctx, 0, &m);
        }
        if (rc != 0)
            throw new OsdpException("No metrics available.");
        return Metrics.FromNative(m);
    }

    /// <summary>Tear down the link. Queued events complete as <see cref="CompletionStatus.Aborted"/>.</summary>
    /// <exception cref="InvalidOperationException">Called from inside <see cref="CommandHandler"/>.</exception>
    public void Dispose()
    {
        ThrowIfInsideCallback(nameof(Dispose));
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        lock (_gate)
        {
            Native.PdTeardown(_ctx);
            _ctx = 0;
        }
        _pending.AbortRemaining();
        _link.Close(nameof(PeripheralDevice));
        _self.Free();
        _channel.Free();
        if (_fileOps.IsAllocated)
            _fileOps.Free();
    }

    // Only the refresh thread holds _gate while user code runs, so holding it
    // here means the handler is calling back in. Refreshing or tearing down
    // the context from inside its own refresh crashes LibOSDP.
    private void ThrowIfInsideCallback(string member)
    {
        if (Monitor.IsEntered(_gate))
            throw new InvalidOperationException(
                $"{member} cannot be called from inside a LibOSDP callback (EventReceived, CommandHandler, a channel or file operation).");
    }

    private static PeripheralDevice? From(nint arg)
    {
        try
        {
            return GCHandle.FromIntPtr(arg).Target as PeripheralDevice;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    [UnmanagedCallersOnly]
    private static int OnCommand(nint arg, NativeCmd* cmd)
    {
        PeripheralDevice? self = From(arg);
        if (self is null)
            return -(int)NakCode.Record;
        try
        {
            OsdpCommand? decoded = CommandMarshal.FromNative(cmd);
            if (decoded is null)
                return 0;
            if (decoded is NotificationCommand note)
            {
                self._link.Apply(0, note.Notification);
                if (!self._forwardNotifications)
                    return 0;
            }
            CommandHandler? handler = self.CommandHandler;
            if (handler is null)
                return 0;
            CommandReply reply = handler(decoded);
            if (reply.Answer is { } answer)
            {
                if (answer.GetType() != decoded.GetType())
                    throw new ArgumentException(
                        $"Answered {decoded.GetType().Name} with {answer.GetType().Name}; the answer must be the same kind.");
                CommandMarshal.ToNative(answer, cmd);
            }
            return reply.NativeResult;
        }
        catch (Exception e)
        {
            self._errors.Record(e);
            return -(int)NakCode.Record;
        }
    }

    [UnmanagedCallersOnly]
    private static void OnEventCompleted(nint arg, NativeEvent* ev, int status) =>
        From(arg)?._pending.Complete(ev, (CompletionStatus)status);
}
