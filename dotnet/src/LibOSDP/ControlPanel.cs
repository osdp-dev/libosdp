// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;
using LibOSDP.Interop;

namespace LibOSDP;

/// <summary>
/// An OSDP control panel driving one or more peripheral devices over a shared
/// bus. Call <see cref="RunAsync"/> (or <see cref="Refresh"/> at least every
/// 50 ms) to service the bus, and always <see cref="Dispose"/> it: LibOSDP
/// holds a handle to this object, so it is never garbage collected otherwise.
/// </summary>
public sealed unsafe class ControlPanel : IDisposable
{
    private readonly object _gate = new();
    private readonly int[] _addresses;
    private readonly bool[] _forwardNotifications;
    private readonly LinkState _link;
    private readonly HandlerErrors _errors = new();
    private readonly PendingSubmissions _pending = new();
    private readonly GCHandle[] _fileOps;
    private GCHandle _self;
    private GCHandle _channel;
    private nint _ctx;
    private int _disposed;

    /// <summary>
    /// A PD reported an event. Raised on the refresh thread while the bus is
    /// locked: keep handlers short, and never block on a task that needs the
    /// bus to make progress. Handlers must not call <see cref="Refresh"/> or
    /// <see cref="Dispose"/>; both throw <see cref="InvalidOperationException"/>
    /// there, which then surfaces from <see cref="Refresh"/>.
    /// </summary>
    public event EventHandler<OsdpEventArgs>? EventReceived;

    /// <summary>Set up a control panel.</summary>
    /// <param name="bus">The channel all PDs share.</param>
    /// <param name="pds">The PDs on the bus; addresses must be unique.</param>
    public ControlPanel(OsdpChannel bus, IReadOnlyList<PdInfo> pds)
    {
        ArgumentNullException.ThrowIfNull(bus);
        ArgumentNullException.ThrowIfNull(pds);
        if (pds.Count == 0)
            throw new ArgumentException("A control panel needs at least one PD.", nameof(pds));
        if (pds.Any(p => p is null))
            throw new ArgumentException("The PD list must not contain null.", nameof(pds));
        _addresses = pds.Select(p => p.Address).ToArray();
        if (_addresses.Distinct().Count() != _addresses.Length)
            throw new ArgumentException("PD addresses must be unique.", nameof(pds));
        Osdp.EnsureInitialized();

        _fileOps = new GCHandle[pds.Count];
        _forwardNotifications = pds.Select(p => p.Flags.HasFlag(OsdpFlags.EnableNotification)).ToArray();
        _link = new LinkState(pds.Count);
        _self = GCHandle.Alloc(this);
        _channel = GCHandle.Alloc(new ChannelHost(bus, _errors));
        NativeChannel channel = ChannelBridge.Describe(_channel);
        try
        {
            // Notifications feed IsOnline and friends, so LibOSDP always sends
            // them; the caller's flag decides whether EventReceived sees them.
            using var infos = new NativePdInfoList(pds, OsdpFlags.EnableNotification, capabilities: null);
            _ctx = Native.CpSetup(&channel, pds.Count, infos.Pointer);
            if (_ctx == 0)
                throw new OsdpException("osdp_cp_setup failed; check the PD list.");
        }
        catch
        {
            _self.Free();
            _channel.Free();
            throw;
        }
        nint self = GCHandle.ToIntPtr(_self);
        // Nothing has refreshed the new context yet, so neither is refused.
        _ = Native.CpSetEventCallback(_ctx, &OnEvent, self);
        _ = Native.CpSetCommandCompletionCallback(_ctx, &OnCommandCompleted, self);
    }

    /// <summary>Service the bus once. Rethrows anything a handler or the channel threw.</summary>
    /// <exception cref="InvalidOperationException">Called from inside <see cref="EventReceived"/>.</exception>
    public void Refresh()
    {
        ThrowIfInsideCallback(nameof(Refresh));
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            Native.CpRefresh(_ctx);
        }
        _errors.ThrowIfAny();
    }

    /// <summary>
    /// Service the bus every <see cref="Osdp.RefreshInterval"/> until cancelled
    /// (throws <see cref="OperationCanceledException"/>) or disposed (returns).
    /// </summary>
    /// <param name="cancellationToken">Stops the loop.</param>
    /// <returns>A task that faults if a handler or the channel threw.</returns>
    public Task RunAsync(CancellationToken cancellationToken = default) =>
        RefreshLoop.RunAsync(Refresh, () => Volatile.Read(ref _disposed) != 0, cancellationToken);

    /// <summary>
    /// Queue a command. The returned task completes when LibOSDP is done with
    /// it: answered, failed, flushed, or aborted by <see cref="Dispose"/>.
    /// </summary>
    /// <param name="address">Address of the target PD.</param>
    /// <param name="command">The command.</param>
    /// <returns>How the command settled.</returns>
    /// <exception cref="ArgumentException">Unknown address or invalid command.</exception>
    /// <exception cref="OsdpException">The PD cannot take it now (offline, or the command needs a secure channel).</exception>
    public Task<CompletionStatus> SubmitCommandAsync(int address, OsdpCommand command)
    {
        ArgumentNullException.ThrowIfNull(command);
        int pd = IndexOf(address);
        var cmd = (NativeCmd*)NativeHeap.AllocZeroed((nuint)sizeof(NativeCmd));
        try
        {
            CommandMarshal.ToNative(command, cmd);
        }
        catch
        {
            NativeHeap.Free(cmd);
            throw;
        }
        Task<CompletionStatus> completion = _pending.Track(cmd, sizeof(NativeCmd));
        int rc;
        lock (_gate)
            rc = _ctx == 0 ? int.MinValue : Native.CpSubmitCommand(_ctx, pd, cmd);
        if (rc == 0)
            return completion;
        _pending.Discard(cmd);
        ObjectDisposedException.ThrowIf(rc == int.MinValue, this);
        throw new OsdpException($"PD {address} cannot take {command.GetType().Name} now.");
    }

    /// <summary>Whether the PD is online.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>True when online.</returns>
    public bool IsOnline(int address) => _link.IsOnline(IndexOf(address));

    /// <summary>Whether the PD's secure channel is active.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>True when active.</returns>
    public bool IsSecureChannelActive(int address) => _link.IsSecure(IndexOf(address));

    /// <summary>Wait until the PD is online.</summary>
    /// <param name="address">PD address.</param>
    /// <param name="timeout">How long to wait.</param>
    /// <param name="cancellationToken">Stops waiting.</param>
    /// <returns>False on timeout.</returns>
    public Task<bool> WaitOnlineAsync(int address, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        int pd = IndexOf(address);
        return _link.WaitAsync(() => _link.IsOnline(pd), timeout, cancellationToken);
    }

    /// <summary>Wait until the PD's secure channel is active.</summary>
    /// <param name="address">PD address.</param>
    /// <param name="timeout">How long to wait.</param>
    /// <param name="cancellationToken">Stops waiting.</param>
    /// <returns>False on timeout.</returns>
    public Task<bool> WaitSecureChannelAsync(int address, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        int pd = IndexOf(address);
        return _link.WaitAsync(() => _link.IsSecure(pd), timeout, cancellationToken);
    }

    /// <summary>Drop the PD's queued commands; each completes as <see cref="CompletionStatus.Flushed"/>.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>How many were dropped.</returns>
    public int FlushCommands(int address) => Math.Max(Call(address, Native.CpFlushCommands), 0);

    /// <summary>
    /// Ask a running multipart operation (file transfer, PIV, biometric read) to stop.
    /// Cooperative: the operation ends on a later refresh.
    /// </summary>
    /// <param name="address">PD address.</param>
    /// <param name="what">Which operation.</param>
    /// <returns>False when none was running.</returns>
    public bool Cancel(int address, MpMsgType what) =>
        Call(address, (ctx, pd) => Native.CpCancel(ctx, pd, (int)what)) == 0;

    /// <summary>Set flags on a PD at run time.</summary>
    /// <param name="address">PD address.</param>
    /// <param name="flags">Flags to set.</param>
    /// <returns>False if LibOSDP does not allow changing them at run time.</returns>
    public bool SetFlag(int address, OsdpFlags flags) =>
        Call(address, (ctx, pd) => Native.CpModifyFlag(ctx, pd, (uint)flags, 1)) == 0;

    /// <summary>Clear flags on a PD at run time.</summary>
    /// <param name="address">PD address.</param>
    /// <param name="flags">Flags to clear.</param>
    /// <returns>False if LibOSDP does not allow changing them at run time.</returns>
    public bool ClearFlag(int address, OsdpFlags flags) =>
        Call(address, (ctx, pd) => Native.CpModifyFlag(ctx, pd, (uint)flags, 0)) == 0;

    /// <summary>Resume talking to a disabled PD. Takes effect on the next refresh.</summary>
    /// <param name="address">PD address.</param>
    public void EnablePd(int address) => _ = Call(address, Native.CpEnablePd);

    /// <summary>Stop polling a PD, e.g. while it is serviced. Takes effect on the next refresh.</summary>
    /// <param name="address">PD address.</param>
    public void DisablePd(int address) => _ = Call(address, Native.CpDisablePd);

    /// <summary>Whether the PD is enabled; reflects <see cref="DisablePd"/> and <see cref="EnablePd"/> once refreshed.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>True when enabled.</returns>
    public bool IsPdEnabled(int address) => Call(address, (ctx, pd) => Native.CpIsPdEnabled(ctx, pd)) != 0;

    /// <summary>
    /// Replace a disabled PD's secure channel base key, e.g. to try another key
    /// after a key rotation that never settled. Enable the PD again to use it.
    /// </summary>
    /// <param name="address">PD address.</param>
    /// <param name="scbk">The key; exactly 16 bytes.</param>
    /// <returns>False when the PD is not disabled.</returns>
    public bool SetScbk(int address, byte[] scbk)
    {
        ArgumentNullException.ThrowIfNull(scbk);
        if (scbk.Length != 16)
            throw new ArgumentException("An SCBK is exactly 16 bytes.", nameof(scbk));
        int pd = IndexOf(address);
        int rc;
        fixed (byte* key = scbk)
        {
            lock (_gate)
            {
                ObjectDisposedException.ThrowIf(_ctx == 0, this);
                rc = Native.CpSetScbk(_ctx, pd, key);
            }
        }
        return rc == 0;
    }

    /// <summary>
    /// Watch a TRS-capable PD for smart cards between card sessions, for readers
    /// that do not announce them in their default mode. A sighting arrives as a
    /// <see cref="TrsCardInfoEvent"/> or <see cref="TrsCardPresentEvent"/>, and
    /// transparent mode is then held briefly so that a <see cref="TrsStartCommand"/>
    /// submitted in response adopts the reader as-is.
    /// </summary>
    /// <param name="address">PD address.</param>
    /// <param name="parameters">Scan cadence; null, or zero fields, select the defaults.</param>
    /// <returns>False if LibOSDP refused it.</returns>
    public bool TrsScanEnable(int address, TrsScanParams? parameters = null)
    {
        TrsScanParams p = parameters ?? new TrsScanParams();
        NativeTrsScanParams native = new()
        {
            Mode0DwellMs = p.Mode0DwellMs, Mode1DwellMs = p.Mode1DwellMs, HoldMs = p.HoldMs, Reader = p.Reader,
        };
        int pd = IndexOf(address);
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = Native.CpTrsScanEnable(_ctx, pd, &native);
        }
        return rc == 0;
    }

    /// <summary>Stop the smart-card presence scan on a PD.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>False if LibOSDP refused it.</returns>
    public bool TrsScanDisable(int address) => Call(address, Native.CpTrsScanDisable) == 0;

    /// <summary>
    /// Largest C-APDU a <see cref="TrsSendApduCommand"/> can carry to a PD: the
    /// packet size in use less framing, so below the 258-byte buffer by
    /// default, and final only once the PD has been online.
    /// </summary>
    /// <param name="address">PD address.</param>
    /// <returns>The length in bytes.</returns>
    public int GetTrsMaxApduLength(int address) => Call(address, Native.CpTrsGetMaxApduLen);

    /// <summary>The identity the PD reported; all zero until it has identified itself.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>The identity.</returns>
    /// <exception cref="OsdpException">LibOSDP refused to report it.</exception>
    public PdId GetPdId(int address)
    {
        int pd = IndexOf(address);
        NativePdId id;
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = Native.CpGetPdId(_ctx, pd, &id);
        }
        if (rc != 0)
            throw new OsdpException($"No identity available for PD {address}.");
        return PdId.FromNative(id);
    }

    /// <summary>A capability the PD advertised, or null if it did not (LibOSDP reports an unadvertised one as all zero).</summary>
    /// <param name="address">PD address.</param>
    /// <param name="capability">Which capability.</param>
    /// <returns>The capability entry.</returns>
    public PdCapability? GetCapability(int address, Capability capability)
    {
        int pd = IndexOf(address);
        NativePdCap cap = new() { FunctionCode = (int)capability };
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = Native.CpGetCapability(_ctx, pd, &cap);
        }
        return rc == 0 && (cap.ComplianceLevel != 0 || cap.NumItems != 0)
            ? new PdCapability(capability, cap.ComplianceLevel, cap.NumItems)
            : null;
    }

    /// <summary>Provide the file a <see cref="FileTransferCommand"/> sends to this PD.</summary>
    /// <param name="address">PD address.</param>
    /// <param name="operations">The file source.</param>
    /// <exception cref="InvalidOperationException">The bus has already been refreshed.</exception>
    /// <remarks>Call before the first <see cref="Refresh"/> or <see cref="RunAsync"/>.</remarks>
    public void RegisterFileOperations(int address, IFileOperations operations)
    {
        ArgumentNullException.ThrowIfNull(operations);
        int pd = IndexOf(address);
        GCHandle host = GCHandle.Alloc(new FileOpsHost(operations, _errors));
        NativeFileOps ops = FileOpsBridge.Describe(host);
        int rc;
        lock (_gate)
        {
            if (_ctx == 0)
            {
                host.Free();
                throw new ObjectDisposedException(nameof(ControlPanel));
            }
            rc = Native.FileRegisterOps(_ctx, pd, &ops);
            if (rc == 0)
            {
                // LibOSDP copied the ops and dropped the old arg.
                if (_fileOps[pd].IsAllocated)
                    _fileOps[pd].Free();
                _fileOps[pd] = host;
            }
        }
        if (rc != 0)
        {
            host.Free();
            throw new InvalidOperationException("File operations must be registered before the first refresh.");
        }
    }

    /// <summary>Progress of a running file transfer, or null when none is running.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>The progress.</returns>
    public FileTransferStatus? GetFileTransferStatus(int address)
    {
        int pd = IndexOf(address);
        uint size, offset;
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = Native.GetFileTxStatus(_ctx, pd, &size, &offset);
        }
        return rc == 0 ? new FileTransferStatus(size, offset) : null;
    }

    /// <summary>Take the PD's link counters; this resets them.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>The counters.</returns>
    public Metrics GetMetrics(int address)
    {
        int pd = IndexOf(address);
        NativeMetrics m;
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = Native.GetMetrics(_ctx, pd, &m);
        }
        if (rc != 0)
            throw new OsdpException($"No metrics for PD {address}.");
        return Metrics.FromNative(m);
    }

    private int Call(int address, Func<nint, int, int> call)
    {
        int pd = IndexOf(address);
        int rc;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            rc = call(_ctx, pd);
        }
        return rc;
    }

    /// <summary>Tear down the bus. Queued commands complete as <see cref="CompletionStatus.Aborted"/>.</summary>
    /// <exception cref="InvalidOperationException">Called from inside <see cref="EventReceived"/>.</exception>
    public void Dispose()
    {
        ThrowIfInsideCallback(nameof(Dispose));
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        lock (_gate)
        {
            Native.CpTeardown(_ctx); // runs the Aborted completions
            _ctx = 0;
        }
        _pending.AbortRemaining();
        _link.Close(nameof(ControlPanel));
        _self.Free();
        _channel.Free();
        foreach (GCHandle handle in _fileOps)
        {
            if (handle.IsAllocated)
                handle.Free();
        }
    }

    internal int IndexOf(int address)
    {
        int index = Array.IndexOf(_addresses, address);
        if (index < 0)
            throw new ArgumentException($"No PD with address {address} on this bus.", nameof(address));
        return index;
    }

    // Only the refresh thread holds _gate while user code runs, so holding it
    // here means a handler is calling back in. Refreshing or tearing down the
    // context from inside its own refresh crashes LibOSDP.
    private void ThrowIfInsideCallback(string member)
    {
        if (Monitor.IsEntered(_gate))
            throw new InvalidOperationException(
                $"{member} cannot be called from inside a LibOSDP callback (EventReceived, CommandHandler, a channel or file operation).");
    }

    private static ControlPanel? From(nint arg)
    {
        try
        {
            return GCHandle.FromIntPtr(arg).Target as ControlPanel;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    [UnmanagedCallersOnly]
    private static int OnEvent(nint arg, int pd, NativeEvent* ev)
    {
        ControlPanel? self = From(arg);
        if (self is null)
            return -1;
        try
        {
            OsdpEvent? decoded = EventMarshal.FromNative(ev);
            if (decoded is null)
                return 0;
            if (decoded is NotificationEvent note)
            {
                self._link.Apply(pd, note.Notification);
                if (!self._forwardNotifications[pd])
                    return 0;
            }
            self.EventReceived?.Invoke(self, new OsdpEventArgs(self._addresses[pd], decoded));
            return 0;
        }
        catch (Exception e)
        {
            self._errors.Record(e);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    private static void OnCommandCompleted(nint arg, int pd, NativeCmd* cmd, int status) =>
        From(arg)?._pending.Complete(cmd, (CompletionStatus)status);
}
