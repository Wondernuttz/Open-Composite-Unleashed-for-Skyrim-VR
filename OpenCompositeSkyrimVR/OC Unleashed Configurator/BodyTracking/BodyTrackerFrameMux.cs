using System;

namespace OpenCompositeConfigurator.BodyTracking
{
    public readonly struct BodyTrackerMuxSelection
    {
        internal BodyTrackerMuxSelection(
            BodyTrackerFrame frame,
            BodyTrackerSource source,
            uint outputSourceEpoch,
            bool sourceChanged)
        {
            Frame = frame;
            Source = source;
            OutputSourceEpoch = outputSourceEpoch;
            SourceChanged = sourceChanged;
        }

        public BodyTrackerFrame Frame { get; }
        public BodyTrackerSource Source { get; }

        /// <summary>
        /// Mux-owned epoch for the OSC frame header. It changes whenever the
        /// selected source or that source's own epoch changes.
        /// </summary>
        public uint OutputSourceEpoch { get; }

        public bool SourceChanged { get; }
    }

    /// <summary>
    /// Prefers continuous 3D only after three distinct consecutive valid
    /// frames. Once acquired, the last good frame survives a brief invalid or
    /// quiet inference for at most 250 ms. That grace prevents a single model
    /// miss from resetting body alignment twice. A real timeout or source-epoch
    /// change drops the primary, and reacquisition requires three new frames.
    /// </summary>
    public sealed class BodyTrackerFrameMux
    {
        public const int DefaultAcquireFrameCount = 3;
        public const int DefaultStaleAfterMilliseconds = 250;

        private readonly int _acquireFrameCount;
        private readonly int _staleAfterMs;

        private bool _haveObservedPrimary;
        private long _lastObservedPrimarySequence;
        private long _lastObservedPrimaryTimestamp;
        private uint _lastObservedPrimaryEpoch;
        private int _primaryValidStreak;
        private bool _primaryAcquired;
        private BodyTrackerFrame? _lastGoodPrimary;

        private BodyTrackerSource _activeSource;
        private uint _activeInputEpoch;
        private uint _outputEpoch;

        public BodyTrackerFrameMux(
            int acquireFrameCount = DefaultAcquireFrameCount,
            int staleAfterMilliseconds = DefaultStaleAfterMilliseconds)
        {
            if (acquireFrameCount < 1)
                throw new ArgumentOutOfRangeException(nameof(acquireFrameCount));
            if (staleAfterMilliseconds < 1)
                throw new ArgumentOutOfRangeException(nameof(staleAfterMilliseconds));
            _acquireFrameCount = acquireFrameCount;
            _staleAfterMs = staleAfterMilliseconds;
        }

        public BodyTrackerSource ActiveSource => _activeSource;
        public int PrimaryAcquireProgress =>
            Math.Min(_primaryValidStreak, _acquireFrameCount);
        public bool PrimaryAcquired => _primaryAcquired;
        public uint OutputSourceEpoch => _outputEpoch;

        public void Reset(uint initialOutputEpoch = 0)
        {
            if (initialOutputEpoch > OscTrackerFrameContract.MaximumExactFloatInteger)
                throw new ArgumentOutOfRangeException(nameof(initialOutputEpoch));
            _haveObservedPrimary = false;
            _lastObservedPrimarySequence = 0;
            _lastObservedPrimaryTimestamp = 0;
            _lastObservedPrimaryEpoch = 0;
            _primaryValidStreak = 0;
            _primaryAcquired = false;
            _lastGoodPrimary = null;
            _activeSource = BodyTrackerSource.None;
            _activeInputEpoch = 0;
            _outputEpoch = initialOutputEpoch;
        }

        public bool TrySelect(
            long nowMs,
            BodyTrackerFrame? continuous3D,
            BodyTrackerFrame? fallback2D,
            out BodyTrackerMuxSelection selection)
        {
            bool newPrimary = IsNewPrimaryFrame(continuous3D);
            if (newPrimary)
                ObservePrimary(nowMs, continuous3D!);

            BodyTrackerFrame? selected = null;
            BodyTrackerSource selectedSource = BodyTrackerSource.None;

            if (_primaryAcquired && _lastGoodPrimary != null)
            {
                if (!IsFresh(nowMs, _lastGoodPrimary))
                {
                    DropPrimary();
                }
                else
                {
                    selected = _lastGoodPrimary;
                    selectedSource = BodyTrackerSource.Continuous3D;
                }
            }

            if (selected == null && IsEligibleFallback(nowMs, fallback2D))
            {
                selected = fallback2D;
                selectedSource = BodyTrackerSource.Legacy2D;
            }

            if (selected == null)
            {
                _activeSource = BodyTrackerSource.None;
                _activeInputEpoch = 0;
                selection = default;
                return false;
            }

            bool sourceChanged = selectedSource != _activeSource
                || selected.SourceEpoch != _activeInputEpoch;
            if (sourceChanged)
            {
                AdvanceOutputEpoch();
                _activeSource = selectedSource;
                _activeInputEpoch = selected.SourceEpoch;
            }

            selection = new BodyTrackerMuxSelection(
                selected, selectedSource, _outputEpoch, sourceChanged);
            return true;
        }

        private bool IsNewPrimaryFrame(BodyTrackerFrame? frame)
        {
            if (frame == null)
                return false;
            if (!_haveObservedPrimary)
                return true;
            if (frame.SourceEpoch != _lastObservedPrimaryEpoch)
                return true;
            if (frame.Sequence > _lastObservedPrimarySequence)
                return true;
            return frame.Sequence == _lastObservedPrimarySequence
                && frame.TimestampMs > _lastObservedPrimaryTimestamp;
        }

        private void ObservePrimary(long nowMs, BodyTrackerFrame frame)
        {
            bool sameEpoch = _haveObservedPrimary
                && frame.SourceEpoch == _lastObservedPrimaryEpoch;
            bool epochChanged = _haveObservedPrimary && !sameEpoch;
            bool monotonic = sameEpoch
                && (frame.Sequence > _lastObservedPrimarySequence
                    || (frame.Sequence == _lastObservedPrimarySequence
                        && frame.TimestampMs > _lastObservedPrimaryTimestamp));
            bool consecutive = monotonic
                && frame.TimestampMs >= _lastObservedPrimaryTimestamp
                && frame.TimestampMs - _lastObservedPrimaryTimestamp <= _staleAfterMs;

            _haveObservedPrimary = true;
            _lastObservedPrimarySequence = frame.Sequence;
            _lastObservedPrimaryTimestamp = frame.TimestampMs;
            _lastObservedPrimaryEpoch = frame.SourceEpoch;

            // Never carry an acquired pose through a worker restart. Its local
            // camera origin may have changed, so the new epoch must prove three
            // stable frames before it can own the tracker stream.
            if (epochChanged)
                DropPrimary();

            if (!IsEligiblePrimary(nowMs, frame))
            {
                _primaryValidStreak = 0;
                // A same-epoch inference miss is ordinary monocular-camera
                // noise. Hold the last complete skeleton through the freshness
                // window; TrySelect drops it once that window actually expires.
                if (!_primaryAcquired || _lastGoodPrimary == null
                    || !IsFresh(nowMs, _lastGoodPrimary))
                    DropPrimary();
                return;
            }

            _primaryValidStreak = consecutive ? _primaryValidStreak + 1 : 1;
            _lastGoodPrimary = frame;
            if (_primaryValidStreak >= _acquireFrameCount)
                _primaryAcquired = true;
        }

        private bool IsEligiblePrimary(long nowMs, BodyTrackerFrame frame) =>
            frame.Source == BodyTrackerSource.Continuous3D
            && (frame.SourceFlags & BodyPoseSourceFlags.Continuous3D) != 0
            && frame.CorePoseValid
            && IsFresh(nowMs, frame);

        private bool IsEligibleFallback(long nowMs, BodyTrackerFrame? frame) =>
            frame != null
            && frame.Source == BodyTrackerSource.Legacy2D
            && frame.CorePoseValid
            && IsFresh(nowMs, frame);

        private bool IsFresh(long nowMs, BodyTrackerFrame frame)
        {
            long age = nowMs - frame.TimestampMs;
            // A tiny forward skew is tolerated when capture and UI clocks are
            // sampled on adjacent threads. Larger future timestamps violate the
            // monotonic-clock contract and are rejected.
            return age >= -50 && age <= _staleAfterMs;
        }

        private void DropPrimary()
        {
            _primaryValidStreak = 0;
            _primaryAcquired = false;
            _lastGoodPrimary = null;
        }

        private void AdvanceOutputEpoch()
        {
            _outputEpoch++;
            if (_outputEpoch == 0
                || _outputEpoch > OscTrackerFrameContract.MaximumExactFloatInteger)
                _outputEpoch = 1;
        }
    }
}
