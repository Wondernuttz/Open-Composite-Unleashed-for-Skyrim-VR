using System;
using System.Numerics;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>
    /// Conservative thresholds for the MediaPipe world-landmark leg classifier.
    /// Distances and speeds are normalized by the measured hip-knee-ankle
    /// length, so the same values apply to both legs and different body sizes.
    /// </summary>
    public sealed class World3DLegMotionClassifierOptions
    {
        public float MinimumLandmarkConfidence { get; set; } = 0.25f;
        public int NeutralWarmupFrames { get; set; } = 8;
        public float NeutralMinimumKneeAngleDegrees { get; set; } = 158f;
        public float NeutralAdaptation { get; set; } = 0.012f;

        public float GroundedReleaseLift { get; set; } = 0.035f;
        public float LiftOnset { get; set; } = 0.070f;
        public float ChamberMinimumKneeLift { get; set; } = 0.135f;
        public float ChamberMaximumKneeAngleDegrees { get; set; } = 148f;
        public int KneeHoldMilliseconds { get; set; } = 190;

        public float KickMinimumLift { get; set; } = 0.075f;
        public float KickMinimumKneeAngleDegrees { get; set; } = 151f;
        public float KickMinimumAngleGainDegrees { get; set; } = 20f;
        public float KickMinimumReachGain { get; set; } = 0.075f;
        public float KickMinimumAngleRateDegreesPerSecond { get; set; } = 55f;
        public float KickMinimumReachRatePerSecond { get; set; } = 0.30f;
        public float KickMinimumDistalDominancePerSecond { get; set; } = 0.10f;
        public float DirectKickMinimumReachGain { get; set; } = 0.20f;
        public float DirectKickMinimumReachRatePerSecond { get; set; } = 0.62f;
        public int KickMaximumExtensionMilliseconds { get; set; } = 700;
        public int RecoveryMilliseconds { get; set; } = 450;

        public int MinimumStepMilliseconds { get; set; } = 110;
        public int MaximumStepMilliseconds { get; set; } = 1550;
        public int MinimumStepIntervalMilliseconds { get; set; } = 170;
        public int MaximumStepIntervalMilliseconds { get; set; } = 1700;
        public int AlternationsRequiredForGait { get; set; } = 2;
        public int GaitLockMilliseconds { get; set; } = 1700;
        public int PostActionQuarantineMilliseconds { get; set; } = 550;
        public int BothFeetQuietMilliseconds { get; set; } = 180;
        public int MaximumFrameGapMilliseconds { get; set; } = 300;
        public float VelocityFilterTimeConstantSeconds { get; set; } = 0.055f;
    }

    /// <summary>
    /// World3D-authoritative lower-body state machine. It consumes MediaPipe's
    /// metric hip/knee/ankle skeleton directly; normalized image landmarks are
    /// intentionally not used. A lift is kept Undecided until it completes as
    /// an ordinary step or proves itself to be a chamber/kick. Gait is armed
    /// only by completed, alternating, non-action episodes. Repeated motion on
    /// one leg therefore cannot become forward locomotion.
    /// </summary>
    public sealed class World3DLegMotionClassifier
    {
        private sealed class LegTrack
        {
            public CameraLegMotionState State = CameraLegMotionState.Invalid;
            public long StateSinceMs;
            public long LastAcceptedMs = -1;

            public bool BaselineReady;
            public int NeutralSamples;
            public float NeutralLegLength;
            public float NeutralKneeUp;
            public float NeutralAnkleUp;
            public float NeutralReach;
            public int NeutralFootFloorSamples;
            public float NeutralFootFloorY;
            public int NeutralKneeFloorSamples;
            public float NeutralKneeFloorY;

            public bool PreviousValid;
            public Vector3 PreviousKneeLocal;
            public Vector3 PreviousAnkleLocal;
            public float PreviousKneeAngle;
            public float PreviousReach;
            public float FilteredKneeSpeed;
            public float FilteredAnkleSpeed;
            public float FilteredAngleRate;
            public float FilteredReachRate;

            public bool EpisodeActive;
            public bool EpisodeEligible;
            public bool EpisodeWasAction;
            public long EpisodeStartedMs;
            public float EpisodePeakLift;
            public float EpisodeMinimumAngle = 180f;
            public float EpisodeMinimumReach = 1f;
            public long RecoverUntilMs;
        }

        private readonly struct BodyBasis
        {
            public BodyBasis(Vector3 right, Vector3 up, Vector3 forward)
            {
                Right = right;
                Up = up;
                Forward = forward;
            }

            public Vector3 Right { get; }
            public Vector3 Up { get; }
            public Vector3 Forward { get; }

            public Vector3 ToLocal(Vector3 value) => new Vector3(
                Vector3.Dot(value, Right),
                Vector3.Dot(value, Up),
                Vector3.Dot(value, Forward));
        }

        private readonly struct LegMeasurement
        {
            public LegMeasurement(
                bool valid,
                bool baselineReady,
                float confidence,
                float kneeAngle,
                float straightness,
                float ankleLift,
                float kneeLift,
                float reach,
                float reachDelta,
                float forwardDelta,
                float kneeSpeed,
                float ankleSpeed,
                float angleRate,
                float reachRate,
                float distalDominance)
            {
                Valid = valid;
                BaselineReady = baselineReady;
                Confidence = confidence;
                KneeAngle = kneeAngle;
                Straightness = straightness;
                AnkleLift = ankleLift;
                KneeLift = kneeLift;
                Reach = reach;
                ReachDelta = reachDelta;
                ForwardDelta = forwardDelta;
                KneeSpeed = kneeSpeed;
                AnkleSpeed = ankleSpeed;
                AngleRate = angleRate;
                ReachRate = reachRate;
                DistalDominance = distalDominance;
            }

            public bool Valid { get; }
            public bool BaselineReady { get; }
            public float Confidence { get; }
            public float KneeAngle { get; }
            public float Straightness { get; }
            public float AnkleLift { get; }
            public float KneeLift { get; }
            public float Reach { get; }
            public float ReachDelta { get; }
            public float ForwardDelta { get; }
            public float KneeSpeed { get; }
            public float AnkleSpeed { get; }
            public float AngleRate { get; }
            public float ReachRate { get; }
            public float DistalDominance { get; }

            public static LegMeasurement Invalid => default;
        }

        private readonly struct AdvanceResult
        {
            public AdvanceResult(bool stepCompleted, bool actionActive, bool liftOnset)
            {
                StepCompleted = stepCompleted;
                ActionActive = actionActive;
                LiftOnset = liftOnset;
            }

            public bool StepCompleted { get; }
            public bool ActionActive { get; }
            public bool LiftOnset { get; }
        }

        private readonly LegTrack[] _legs = { new LegTrack(), new LegTrack() };
        private long _lastTimestampMs = -1;
        private uint _sourceEpoch;
        private bool _sourceEpochValid;
        private long _lastStepMs = -1;
        private int _lastStepSide = -1;
        private int _alternationCount;
        private long _gaitLockedUntilMs;
        private long _actionQuarantineUntilMs;
        private long _bothGroundedSinceMs = -1;

        public World3DLegMotionClassifier(
            World3DLegMotionClassifierOptions? options = null)
        {
            Options = options ?? new World3DLegMotionClassifierOptions();
        }

        public World3DLegMotionClassifierOptions Options { get; }

        public void Reset()
        {
            _legs[0] = new LegTrack();
            _legs[1] = new LegTrack();
            _lastTimestampMs = -1;
            _sourceEpochValid = false;
            _sourceEpoch = 0;
            ClearGaitEvidence();
            _actionQuarantineUntilMs = 0;
            _bothGroundedSinceMs = -1;
        }

        /// <summary>
        /// Classifies directly from a MediaPipe world-landmark frame.
        /// </summary>
        public CameraLowerBodyClassification Update(WorldLandmarkFrame landmarks) =>
            UpdateCore(landmarks, null);

        /// <summary>
        /// Classifies a world-landmark frame synchronized with its converted
        /// tracker frame. Joint angle/extension geometry comes from the metric
        /// 33-point skeleton; floor-relative foot/knee lift comes from the same
        /// converted tracker poses that are transmitted to the runtime.
        /// </summary>
        public CameraLowerBodyClassification Update(
            WorldLandmarkFrame landmarks,
            BodyTrackerFrame synchronizedTrackers) =>
            UpdateCore(landmarks, synchronizedTrackers);

        private CameraLowerBodyClassification UpdateCore(
            WorldLandmarkFrame landmarks,
            BodyTrackerFrame? synchronizedTrackers)
        {
            ArgumentNullException.ThrowIfNull(landmarks);
            long nowMs = landmarks.TimestampMs;

            if (synchronizedTrackers != null
                && (synchronizedTrackers.Source != BodyTrackerSource.Continuous3D
                    || synchronizedTrackers.Sequence != landmarks.Sequence
                    || synchronizedTrackers.SourceEpoch != landmarks.SourceEpoch
                    || synchronizedTrackers.TimestampMs != landmarks.TimestampMs
                    || !LowerBodyTrackerContractValid(synchronizedTrackers)))
            {
                return InvalidClassification(nowMs);
            }

            if (!_sourceEpochValid || landmarks.SourceEpoch != _sourceEpoch)
            {
                Reset();
                _sourceEpoch = landmarks.SourceEpoch;
                _sourceEpochValid = true;
            }
            else if (_lastTimestampMs >= 0 && nowMs <= _lastTimestampMs)
            {
                return InvalidClassification(nowMs);
            }
            else if (_lastTimestampMs >= 0
                && nowMs - _lastTimestampMs > Options.MaximumFrameGapMilliseconds)
            {
                Reset();
                _sourceEpoch = landmarks.SourceEpoch;
                _sourceEpochValid = true;
            }
            _lastTimestampMs = nowMs;

            if (!TryBuildBodyBasis(landmarks, out BodyBasis basis))
                return InvalidClassification(nowMs);

            LegMeasurement left = MeasureLeg(
                landmarks, basis, _legs[0], true, synchronizedTrackers, nowMs);
            LegMeasurement right = MeasureLeg(
                landmarks, basis, _legs[1], false, synchronizedTrackers, nowMs);

            AdvanceResult leftAdvance = AdvanceLeg(_legs[0], left, nowMs);
            AdvanceResult rightAdvance = AdvanceLeg(_legs[1], right, nowMs);
            bool actionActive = leftAdvance.ActionActive || rightAdvance.ActionActive;

            bool bothGrounded = IsGrounded(_legs[0], left)
                && IsGrounded(_legs[1], right);
            if (bothGrounded)
            {
                if (_bothGroundedSinceMs < 0)
                    _bothGroundedSinceMs = nowMs;
            }
            else
            {
                _bothGroundedSinceMs = -1;
            }

            if (actionActive)
            {
                ClearGaitEvidence();
                _actionQuarantineUntilMs = Math.Max(
                    _actionQuarantineUntilMs,
                    nowMs + Options.PostActionQuarantineMilliseconds);
            }
            else
            {
                bool quietAfterAction = nowMs >= _actionQuarantineUntilMs
                    && (_actionQuarantineUntilMs == 0
                        || (_bothGroundedSinceMs >= 0
                            && nowMs - _bothGroundedSinceMs
                                >= Options.BothFeetQuietMilliseconds));
                if (quietAfterAction)
                    RegisterCompletedSteps(leftAdvance, rightAdvance, nowMs);
            }

            bool gaitActive = !actionActive
                && nowMs >= _actionQuarantineUntilMs
                && nowMs <= _gaitLockedUntilMs;
            if (!gaitActive && _gaitLockedUntilMs != 0
                && nowMs > _gaitLockedUntilMs)
            {
                ClearGaitEvidence();
            }

            ApplyGaitState(_legs[0], left, gaitActive, nowMs);
            ApplyGaitState(_legs[1], right, gaitActive, nowMs);

            float gaitConfidence = gaitActive
                ? Math.Clamp(0.48f + 0.18f * _alternationCount, 0.48f, 1f)
                : 0f;
            return new CameraLowerBodyClassification(
                nowMs,
                BuildClassification(_legs[0], left, leftAdvance.LiftOnset, gaitActive),
                BuildClassification(_legs[1], right, rightAdvance.LiftOnset, gaitActive),
                gaitActive,
                gaitConfidence);
        }

        private LegMeasurement MeasureLeg(
            WorldLandmarkFrame frame,
            in BodyBasis basis,
            LegTrack track,
            bool left,
            BodyTrackerFrame? synchronizedTrackers,
            long nowMs)
        {
            MediaPipe33LandmarkIndex hipIndex = left
                ? MediaPipe33LandmarkIndex.LeftHip : MediaPipe33LandmarkIndex.RightHip;
            MediaPipe33LandmarkIndex kneeIndex = left
                ? MediaPipe33LandmarkIndex.LeftKnee : MediaPipe33LandmarkIndex.RightKnee;
            MediaPipe33LandmarkIndex ankleIndex = left
                ? MediaPipe33LandmarkIndex.LeftAnkle : MediaPipe33LandmarkIndex.RightAnkle;

            if (!TryWorldPoint(frame, hipIndex, out Vector3 hip, out float hipConfidence)
                || !TryWorldPoint(frame, kneeIndex, out Vector3 knee, out float kneeConfidence)
                || !TryWorldPoint(frame, ankleIndex, out Vector3 ankle, out float ankleConfidence))
            {
                track.PreviousValid = false;
                return LegMeasurement.Invalid;
            }

            float confidence = Math.Clamp(Math.Min(
                hipConfidence, Math.Min(kneeConfidence, ankleConfidence)), 0f, 1f);
            int footTrackerSlot = left ? 2 : 3;
            int kneeTrackerSlot = left ? 4 : 5;
            bool trackerFootValid = TryTrackerPosition(
                synchronizedTrackers, footTrackerSlot,
                out Vector3 trackerFoot, out float trackerConfidence);
            bool trackerKneeValid = TryTrackerPosition(
                synchronizedTrackers, kneeTrackerSlot,
                out Vector3 trackerKnee, out _);
            if (trackerConfidence >= 0f)
                confidence = Math.Min(confidence, trackerConfidence);

            Vector3 kneeLocal = basis.ToLocal(knee - hip);
            Vector3 ankleLocal = basis.ToLocal(ankle - hip);
            float thighLength = Vector3.Distance(hip, knee);
            float shinLength = Vector3.Distance(knee, ankle);
            float legLength = thighLength + shinLength;
            if (!float.IsFinite(legLength) || thighLength < 0.08f
                || shinLength < 0.08f || legLength < 0.25f)
            {
                track.PreviousValid = false;
                return LegMeasurement.Invalid;
            }

            float kneeAngle = AngleDegrees(hip - knee, ankle - knee);
            float straightness = Math.Clamp(
                Vector3.Distance(hip, ankle) / legLength, 0f, 1f);
            // Once neutral is known, every normalized feature uses that frozen
            // anatomical scale. Dividing by the current inferred length makes a
            // foreshortened raised leg shrink its own lift/reach signal.
            float referenceLength = track.BaselineReady
                ? Math.Max(0.25f, track.NeutralLegLength)
                : legLength;
            float kneeUp = kneeLocal.Y / referenceLength;
            float ankleUp = ankleLocal.Y / referenceLength;
            float reach = MathF.Sqrt(
                ankleLocal.X * ankleLocal.X + ankleLocal.Z * ankleLocal.Z)
                / referenceLength;

            bool neutralCandidate = kneeAngle >= Options.NeutralMinimumKneeAngleDegrees
                && straightness >= 0.965f;
            if (!track.BaselineReady && neutralCandidate)
            {
                AccumulateNeutral(track, legLength, kneeUp, ankleUp, reach,
                    trackerFootValid, trackerFoot.Y,
                    trackerKneeValid, trackerKnee.Y);
            }

            bool useTrackerFootLift = track.BaselineReady
                && trackerFootValid
                && track.NeutralFootFloorSamples >= Options.NeutralWarmupFrames;
            bool useTrackerKneeLift = track.BaselineReady
                && trackerKneeValid
                && track.NeutralKneeFloorSamples >= Options.NeutralWarmupFrames;
            // The synchronized converter has already established one coherent
            // floor-relative skeleton. Its foot/knee Y survives the common hip
            // motion and MediaPipe world-origin changes that can cancel a real
            // lift in hip-relative raw landmarks (the observed right-leg miss).
            float ankleLift = track.BaselineReady
                ? useTrackerFootLift
                    ? (trackerFoot.Y - track.NeutralFootFloorY) / referenceLength
                    : ankleUp - track.NeutralAnkleUp
                : 0f;
            float kneeLift = track.BaselineReady
                ? useTrackerKneeLift
                    ? (trackerKnee.Y - track.NeutralKneeFloorY) / referenceLength
                    : kneeUp - track.NeutralKneeUp
                : 0f;
            float reachDelta = track.BaselineReady
                ? reach - track.NeutralReach : 0f;
            float forwardDelta = track.BaselineReady
                ? ankleLocal.Z / referenceLength : 0f;

            float kneeSpeed = 0f;
            float ankleSpeed = 0f;
            float angleRate = 0f;
            float reachRate = 0f;
            if (track.PreviousValid && track.LastAcceptedMs >= 0)
            {
                float dt = Math.Clamp((nowMs - track.LastAcceptedMs) / 1000f,
                    0.001f, 0.25f);
                float rawKneeSpeed = Vector3.Distance(
                    kneeLocal, track.PreviousKneeLocal) / referenceLength / dt;
                float rawAnkleSpeed = Vector3.Distance(
                    ankleLocal, track.PreviousAnkleLocal) / referenceLength / dt;
                float rawAngleRate = (kneeAngle - track.PreviousKneeAngle) / dt;
                float rawReachRate = (reach - track.PreviousReach) / dt;
                float tau = Math.Max(0.001f, Options.VelocityFilterTimeConstantSeconds);
                float alpha = 1f - MathF.Exp(-dt / tau);
                track.FilteredKneeSpeed += alpha
                    * (rawKneeSpeed - track.FilteredKneeSpeed);
                track.FilteredAnkleSpeed += alpha
                    * (rawAnkleSpeed - track.FilteredAnkleSpeed);
                track.FilteredAngleRate += alpha
                    * (rawAngleRate - track.FilteredAngleRate);
                track.FilteredReachRate += alpha
                    * (rawReachRate - track.FilteredReachRate);
                kneeSpeed = track.FilteredKneeSpeed;
                ankleSpeed = track.FilteredAnkleSpeed;
                angleRate = track.FilteredAngleRate;
                reachRate = track.FilteredReachRate;
            }

            track.PreviousValid = true;
            track.PreviousKneeLocal = kneeLocal;
            track.PreviousAnkleLocal = ankleLocal;
            track.PreviousKneeAngle = kneeAngle;
            track.PreviousReach = reach;
            track.LastAcceptedMs = nowMs;

            bool stableGround = track.BaselineReady
                && kneeAngle >= Options.NeutralMinimumKneeAngleDegrees
                && Math.Abs(ankleLift) <= Options.GroundedReleaseLift
                && Math.Abs(kneeLift) <= Options.GroundedReleaseLift * 1.35f
                && ankleSpeed < 0.22f;
            if (stableGround)
            {
                AdaptNeutral(track, kneeUp, ankleUp, reach,
                    trackerFootValid, trackerFoot.Y,
                    trackerKneeValid, trackerKnee.Y);
            }

            return new LegMeasurement(
                true,
                track.BaselineReady,
                confidence,
                kneeAngle,
                straightness,
                ankleLift,
                kneeLift,
                reach,
                reachDelta,
                forwardDelta,
                kneeSpeed,
                ankleSpeed,
                angleRate,
                reachRate,
                ankleSpeed - 0.68f * kneeSpeed);
        }

        private AdvanceResult AdvanceLeg(
            LegTrack track,
            in LegMeasurement measurement,
            long nowMs)
        {
            if (!measurement.Valid || !measurement.BaselineReady)
            {
                SetState(track, CameraLegMotionState.Invalid, nowMs);
                return default;
            }

            bool grounded = measurement.AnkleLift <= Options.GroundedReleaseLift
                && measurement.KneeLift <= Options.GroundedReleaseLift * 1.35f;
            bool airborne = measurement.AnkleLift >= Options.LiftOnset
                || measurement.KneeLift >= Options.LiftOnset * 1.10f;
            bool liftOnset = false;
            bool stepCompleted = false;

            if (!track.EpisodeActive && airborne)
            {
                track.EpisodeActive = true;
                track.EpisodeEligible = nowMs >= track.RecoverUntilMs;
                track.EpisodeWasAction = false;
                track.EpisodeStartedMs = nowMs;
                track.EpisodePeakLift = Math.Max(
                    measurement.AnkleLift, measurement.KneeLift);
                track.EpisodeMinimumAngle = measurement.KneeAngle;
                track.EpisodeMinimumReach = measurement.Reach;
                liftOnset = true;
                SetState(track, CameraLegMotionState.UndecidedLift, nowMs);
            }

            if (track.EpisodeActive)
            {
                track.EpisodePeakLift = Math.Max(track.EpisodePeakLift,
                    Math.Max(measurement.AnkleLift, measurement.KneeLift));
                track.EpisodeMinimumAngle = Math.Min(
                    track.EpisodeMinimumAngle, measurement.KneeAngle);
                track.EpisodeMinimumReach = Math.Min(
                    track.EpisodeMinimumReach, measurement.Reach);
            }

            bool chamberPose = track.EpisodeActive
                && measurement.KneeLift >= Options.ChamberMinimumKneeLift
                && measurement.KneeAngle <= Options.ChamberMaximumKneeAngleDegrees;
            bool kick = track.EpisodeActive && IsKickExtension(track, measurement);

            if (kick)
            {
                track.EpisodeEligible = false;
                track.EpisodeWasAction = true;
                track.RecoverUntilMs = nowMs + Options.RecoveryMilliseconds;
                SetState(track, CameraLegMotionState.KickExtend, nowMs);
            }
            else if (track.State == CameraLegMotionState.KickExtend)
            {
                bool extensionEnded = grounded
                    || measurement.ReachRate < -0.20f
                    || measurement.KneeAngle
                        < Options.KickMinimumKneeAngleDegrees - 15f
                    || nowMs - track.StateSinceMs
                        >= Options.KickMaximumExtensionMilliseconds;
                if (extensionEnded)
                {
                    track.RecoverUntilMs = Math.Max(track.RecoverUntilMs,
                        nowMs + Options.RecoveryMilliseconds);
                    SetState(track, CameraLegMotionState.Recover, nowMs);
                }
            }
            else if (chamberPose)
            {
                // Chamber is provisional. A normal/high stride can briefly
                // share this pose, so do not poison the completed-step history
                // unless a distal extension actually proves a kick. The state
                // still blocks immediate locomotion while it is ambiguous.
                if (track.State != CameraLegMotionState.Chamber
                    && track.State != CameraLegMotionState.KneeHold)
                {
                    SetState(track, CameraLegMotionState.Chamber, nowMs);
                }
                else if (track.State == CameraLegMotionState.Chamber
                    && nowMs - track.StateSinceMs >= Options.KneeHoldMilliseconds)
                {
                    SetState(track, CameraLegMotionState.KneeHold, nowMs);
                }
            }

            if (track.EpisodeActive && grounded)
            {
                long episodeMs = nowMs - track.EpisodeStartedMs;
                stepCompleted = track.EpisodeEligible
                    && !track.EpisodeWasAction
                    && track.EpisodePeakLift >= Options.LiftOnset
                    && episodeMs >= Options.MinimumStepMilliseconds
                    && episodeMs <= Options.MaximumStepMilliseconds;
                track.EpisodeActive = false;
                track.EpisodeEligible = false;
                if (track.EpisodeWasAction
                    || track.State == CameraLegMotionState.KickExtend)
                {
                    track.RecoverUntilMs = Math.Max(track.RecoverUntilMs,
                        nowMs + Options.RecoveryMilliseconds);
                    SetState(track, CameraLegMotionState.Recover, nowMs);
                }
                else
                {
                    SetState(track, CameraLegMotionState.Grounded, nowMs);
                }
            }
            else if (!track.EpisodeActive && grounded)
            {
                if (track.State == CameraLegMotionState.Recover)
                {
                    if (nowMs >= track.RecoverUntilMs)
                        SetState(track, CameraLegMotionState.Grounded, nowMs);
                }
                else if (track.State != CameraLegMotionState.Grounded)
                {
                    SetState(track, CameraLegMotionState.Grounded, nowMs);
                }
            }

            bool actionActive = IsConfirmedActionState(track.State)
                || (track.EpisodeWasAction && nowMs < track.RecoverUntilMs);
            return new AdvanceResult(stepCompleted, actionActive, liftOnset);
        }

        private bool IsKickExtension(
            LegTrack track,
            in LegMeasurement measurement)
        {
            if (measurement.AnkleLift < Options.KickMinimumLift
                || measurement.KneeAngle < Options.KickMinimumKneeAngleDegrees)
                return false;

            float angleGain = measurement.KneeAngle - track.EpisodeMinimumAngle;
            float reachGain = measurement.Reach - track.EpisodeMinimumReach;
            bool temporalImpulse = measurement.AngleRate
                    >= Options.KickMinimumAngleRateDegreesPerSecond
                || measurement.ReachRate
                    >= Options.KickMinimumReachRatePerSecond;
            bool chamberedExtension = angleGain >= Options.KickMinimumAngleGainDegrees
                && reachGain >= Options.KickMinimumReachGain
                && temporalImpulse
                && measurement.DistalDominance
                    >= Options.KickMinimumDistalDominancePerSecond;

            bool directExtension = measurement.ReachDelta
                    >= Options.DirectKickMinimumReachGain
                && measurement.ReachRate
                    >= Options.DirectKickMinimumReachRatePerSecond
                && measurement.DistalDominance
                    >= Options.KickMinimumDistalDominancePerSecond;
            return chamberedExtension || directExtension;
        }

        private void RegisterCompletedSteps(
            in AdvanceResult left,
            in AdvanceResult right,
            long nowMs)
        {
            int side = left.StepCompleted && !right.StepCompleted
                ? 0 : right.StepCompleted && !left.StepCompleted ? 1 : -1;
            if (side < 0)
                return;

            if (_lastStepMs < 0)
            {
                _lastStepMs = nowMs;
                _lastStepSide = side;
                return;
            }

            long interval = nowMs - _lastStepMs;
            if (side == _lastStepSide)
            {
                // This is the important unilateral-kick/knee-raise rejection:
                // same-side repetitions erase, rather than accumulate, gait.
                ClearGaitEvidence();
                _lastStepMs = nowMs;
                _lastStepSide = side;
                return;
            }

            if (interval < Options.MinimumStepIntervalMilliseconds
                || interval > Options.MaximumStepIntervalMilliseconds)
            {
                ClearGaitEvidence();
                _lastStepMs = nowMs;
                _lastStepSide = side;
                return;
            }

            _alternationCount++;
            _lastStepMs = nowMs;
            _lastStepSide = side;
            if (_alternationCount >= Options.AlternationsRequiredForGait)
                _gaitLockedUntilMs = nowMs + Options.GaitLockMilliseconds;
        }

        private static void ApplyGaitState(
            LegTrack track,
            in LegMeasurement measurement,
            bool gaitActive,
            long nowMs)
        {
            if (!measurement.Valid || IsActionState(track.State))
                return;
            bool airborne = track.EpisodeActive;
            if (gaitActive && airborne)
                SetState(track, CameraLegMotionState.WalkStep, nowMs);
            else if (!gaitActive && track.State == CameraLegMotionState.WalkStep)
                SetState(track, airborne
                    ? CameraLegMotionState.UndecidedLift
                    : CameraLegMotionState.Grounded, nowMs);
        }

        private CameraLegClassification BuildClassification(
            LegTrack track,
            in LegMeasurement measurement,
            bool liftOnset,
            bool gaitActive)
        {
            if (!measurement.Valid)
                return InvalidLegClassification(track);

            bool blocksGait = IsActionState(track.State);
            float stateConfidence = track.State switch
            {
                CameraLegMotionState.Grounded => 0.94f,
                CameraLegMotionState.UndecidedLift => 0.55f,
                CameraLegMotionState.Chamber => 0.78f,
                CameraLegMotionState.KneeHold => 0.88f,
                CameraLegMotionState.WalkStep => gaitActive ? 0.92f : 0.58f,
                CameraLegMotionState.KickExtend => 0.94f,
                CameraLegMotionState.Recover => 0.78f,
                _ => 0f,
            };
            return new CameraLegClassification(
                track.State,
                measurement.KneeAngle,
                Math.Clamp(stateConfidence * measurement.Confidence, 0f, 1f),
                true,
                measurement.BaselineReady,
                false,
                blocksGait,
                liftOnset,
                measurement.AnkleLift,
                measurement.KneeLift,
                measurement.Reach,
                measurement.ReachRate,
                measurement.ForwardDelta,
                -track.NeutralKneeUp,
                -track.NeutralAnkleUp,
                -track.NeutralAnkleUp * track.NeutralLegLength);
        }

        private CameraLowerBodyClassification InvalidClassification(long nowMs) =>
            new CameraLowerBodyClassification(
                nowMs,
                InvalidLegClassification(_legs[0]),
                InvalidLegClassification(_legs[1]),
                false,
                0f);

        private static CameraLegClassification InvalidLegClassification(LegTrack track) =>
            new CameraLegClassification(
                CameraLegMotionState.Invalid,
                0f,
                0f,
                false,
                track.BaselineReady,
                false,
                true,
                false,
                0f,
                0f,
                0f,
                0f,
                0f,
                -track.NeutralKneeUp,
                -track.NeutralAnkleUp,
                -track.NeutralAnkleUp * track.NeutralLegLength);

        private bool TryBuildBodyBasis(
            WorldLandmarkFrame frame,
            out BodyBasis basis)
        {
            if (!TryWorldPoint(frame, MediaPipe33LandmarkIndex.LeftHip,
                    out Vector3 leftHip, out _)
                || !TryWorldPoint(frame, MediaPipe33LandmarkIndex.RightHip,
                    out Vector3 rightHip, out _)
                || !TryWorldPoint(frame, MediaPipe33LandmarkIndex.LeftShoulder,
                    out Vector3 leftShoulder, out _)
                || !TryWorldPoint(frame, MediaPipe33LandmarkIndex.RightShoulder,
                    out Vector3 rightShoulder, out _))
            {
                basis = default;
                return false;
            }

            Vector3 hipCenter = 0.5f * (leftHip + rightHip);
            Vector3 shoulderCenter = 0.5f * (leftShoulder + rightShoulder);
            if (!TryNormalize(shoulderCenter - hipCenter, out Vector3 up))
            {
                basis = default;
                return false;
            }
            Vector3 rightCandidate = rightHip - leftHip;
            rightCandidate -= up * Vector3.Dot(rightCandidate, up);
            if (!TryNormalize(rightCandidate, out Vector3 right)
                || !TryNormalize(Vector3.Cross(right, up), out Vector3 forward))
            {
                basis = default;
                return false;
            }
            basis = new BodyBasis(right, up, forward);
            return true;
        }

        private bool TryWorldPoint(
            WorldLandmarkFrame frame,
            MediaPipe33LandmarkIndex index,
            out Vector3 point,
            out float confidence)
        {
            WorldLandmark landmark = frame[(int)index];
            confidence = landmark.Confidence;
            if (!landmark.IsWorldUsable(Options.MinimumLandmarkConfidence))
            {
                point = Vector3.Zero;
                return false;
            }
            Vector3 raw = landmark.WorldPosition;
            float xSign = frame.InputMirrored ? -1f : 1f;
            point = new Vector3(xSign * raw.X, -raw.Y, -raw.Z);
            return WorldLandmark.IsFinite(point);
        }

        private static bool TryTrackerPosition(
            BodyTrackerFrame? trackers,
            int slot,
            out Vector3 position,
            out float confidence)
        {
            if (trackers == null)
            {
                position = Vector3.Zero;
                confidence = -1f;
                return false;
            }
            BodyTrackerPose pose = trackers.GetTracker(slot);
            position = pose.Position;
            confidence = pose.PositionValid ? pose.Confidence : 0f;
            return pose.PositionValid && WorldLandmark.IsFinite(position);
        }

        private static bool LowerBodyTrackerContractValid(BodyTrackerFrame trackers) =>
            trackers.GetTracker(1).PositionValid
            && trackers.GetTracker(2).PositionValid
            && trackers.GetTracker(3).PositionValid;

        private void AccumulateNeutral(
            LegTrack track,
            float legLength,
            float kneeUp,
            float ankleUp,
            float reach,
            bool trackerFootValid,
            float trackerFootY,
            bool trackerKneeValid,
            float trackerKneeY)
        {
            int n = track.NeutralSamples;
            float alpha = 1f / (n + 1f);
            track.NeutralLegLength += alpha * (legLength - track.NeutralLegLength);
            track.NeutralKneeUp += alpha * (kneeUp - track.NeutralKneeUp);
            track.NeutralAnkleUp += alpha * (ankleUp - track.NeutralAnkleUp);
            track.NeutralReach += alpha * (reach - track.NeutralReach);
            if (trackerFootValid && float.IsFinite(trackerFootY))
            {
                float trackerAlpha = 1f / (track.NeutralFootFloorSamples + 1f);
                track.NeutralFootFloorY += trackerAlpha
                    * (trackerFootY - track.NeutralFootFloorY);
                track.NeutralFootFloorSamples++;
            }
            if (trackerKneeValid && float.IsFinite(trackerKneeY))
            {
                float trackerAlpha = 1f / (track.NeutralKneeFloorSamples + 1f);
                track.NeutralKneeFloorY += trackerAlpha
                    * (trackerKneeY - track.NeutralKneeFloorY);
                track.NeutralKneeFloorSamples++;
            }
            track.NeutralSamples++;
            if (track.NeutralSamples >= Options.NeutralWarmupFrames)
            {
                track.BaselineReady = true;
                SetState(track, CameraLegMotionState.Grounded, track.LastAcceptedMs);
            }
        }

        private void AdaptNeutral(
            LegTrack track,
            float kneeUp,
            float ankleUp,
            float reach,
            bool trackerFootValid,
            float trackerFootY,
            bool trackerKneeValid,
            float trackerKneeY)
        {
            float alpha = Math.Clamp(Options.NeutralAdaptation, 0f, 0.10f);
            // NeutralLegLength is the normalization ruler and deliberately
            // freezes after warmup. Letting a foreshortened raised leg change
            // its own denominator was the source of asymmetric missed lifts.
            track.NeutralKneeUp += alpha * (kneeUp - track.NeutralKneeUp);
            track.NeutralAnkleUp += alpha * (ankleUp - track.NeutralAnkleUp);
            track.NeutralReach += alpha * (reach - track.NeutralReach);
            if (trackerFootValid && float.IsFinite(trackerFootY))
            {
                track.NeutralFootFloorY += alpha
                    * (trackerFootY - track.NeutralFootFloorY);
            }
            if (trackerKneeValid && float.IsFinite(trackerKneeY))
            {
                track.NeutralKneeFloorY += alpha
                    * (trackerKneeY - track.NeutralKneeFloorY);
            }
        }

        private static bool IsGrounded(
            LegTrack track,
            in LegMeasurement measurement) =>
            measurement.Valid
            && measurement.BaselineReady
            && !track.EpisodeActive
            && track.State == CameraLegMotionState.Grounded;

        private static bool IsActionState(CameraLegMotionState state) =>
            state == CameraLegMotionState.Chamber
            || state == CameraLegMotionState.KneeHold
            || state == CameraLegMotionState.KickExtend
            || state == CameraLegMotionState.Recover;

        private static bool IsConfirmedActionState(CameraLegMotionState state) =>
            state == CameraLegMotionState.KickExtend
            || state == CameraLegMotionState.Recover;

        private void ClearGaitEvidence()
        {
            _lastStepMs = -1;
            _lastStepSide = -1;
            _alternationCount = 0;
            _gaitLockedUntilMs = 0;
        }

        private static void SetState(
            LegTrack track,
            CameraLegMotionState state,
            long nowMs)
        {
            if (track.State == state)
                return;
            track.State = state;
            track.StateSinceMs = nowMs;
        }

        private static bool TryNormalize(Vector3 value, out Vector3 normalized)
        {
            float lengthSquared = value.LengthSquared();
            if (!float.IsFinite(lengthSquared) || lengthSquared < 1e-10f)
            {
                normalized = Vector3.Zero;
                return false;
            }
            normalized = value / MathF.Sqrt(lengthSquared);
            return WorldLandmark.IsFinite(normalized);
        }

        private static float AngleDegrees(Vector3 a, Vector3 b)
        {
            float denominator = MathF.Sqrt(a.LengthSquared() * b.LengthSquared());
            if (!float.IsFinite(denominator) || denominator < 1e-8f)
                return 0f;
            float cosine = Math.Clamp(Vector3.Dot(a, b) / denominator, -1f, 1f);
            return MathF.Acos(cosine) * (180f / MathF.PI);
        }
    }
}
