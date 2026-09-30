using System;
using System.Collections.Generic;
using System.Numerics;

namespace OpenCompositeConfigurator
{
    /// <summary>
    /// Semantic lower-body motion reported by the monocular camera solver.
    /// Undecided is intentional: a single raised leg must not become either a
    /// gait step or a kick until the temporal evidence disambiguates it.
    /// </summary>
    public enum CameraLegMotionState : byte
    {
        // Values are part of the OSC contract with NetworkTrackers.cpp.
        Invalid = 0,
        Grounded = 1,
        UndecidedLift = 2,
        Chamber = 3,
        KneeHold = 4,
        WalkStep = 5,
        KickExtend = 6,
        Recover = 7,
    }

    public enum CameraLegSide : byte
    {
        Left = 0,
        Right = 1,
    }

    /// <summary>One normalized image-space landmark (Y increases downward).</summary>
    public readonly struct CameraJoint2D
    {
        public CameraJoint2D(float x, float y, float confidence)
        {
            X = x;
            Y = y;
            Confidence = confidence;
        }

        public float X { get; }
        public float Y { get; }
        public float Confidence { get; }

        internal bool IsFinite =>
            float.IsFinite(X) && float.IsFinite(Y) && float.IsFinite(Confidence);

        internal Vector2 Position => new Vector2(X, Y);
    }

    /// <summary>
    /// Camera and virtual-tracker data for one leg. LiftMeters and DepthMeters
    /// are the current camera solver values. They are interpreted relative to
    /// this leg's learned neutral values, never as global absolute thresholds.
    /// </summary>
    public readonly struct CameraLegObservation
    {
        public CameraLegObservation(
            CameraJoint2D hip,
            CameraJoint2D knee,
            CameraJoint2D ankle,
            float liftMeters,
            float depthMeters,
            bool outputFootValid,
            Vector3 outputFootPosition)
        {
            Hip = hip;
            Knee = knee;
            Ankle = ankle;
            LiftMeters = liftMeters;
            DepthMeters = depthMeters;
            OutputFootValid = outputFootValid;
            OutputFootPosition = outputFootPosition;
        }

        public CameraJoint2D Hip { get; }
        public CameraJoint2D Knee { get; }
        public CameraJoint2D Ankle { get; }
        public float LiftMeters { get; }
        public float DepthMeters { get; }
        public bool OutputFootValid { get; }
        public Vector3 OutputFootPosition { get; }
    }

    /// <summary>One synchronized pair of camera legs.</summary>
    public readonly struct CameraLowerBodyFrame
    {
        public CameraLowerBodyFrame(
            long timestampMs,
            float metersPerImageUnit,
            CameraLegObservation left,
            CameraLegObservation right)
        {
            TimestampMs = timestampMs;
            MetersPerImageUnit = metersPerImageUnit;
            Left = left;
            Right = right;
        }

        public long TimestampMs { get; }
        public float MetersPerImageUnit { get; }
        public CameraLegObservation Left { get; }
        public CameraLegObservation Right { get; }
    }

    /// <summary>
    /// Tunable normalized thresholds. Defaults are deliberately conservative
    /// and were selected against the front and +/-45 degree calibration takes.
    /// Length values are fractions of the learned neutral leg length.
    /// </summary>
    public sealed class CameraLowerBodyClassifierOptions
    {
        public float MinimumHipConfidence { get; set; } = 0.25f;
        public float MinimumLegConfidence { get; set; } = 0.10f;
        public int NeutralWarmupFrames { get; set; } = 8;
        public float NeutralMinimumKneeAngleDegrees { get; set; } = 155f;
        public float NeutralAdaptation { get; set; } = 0.025f;
        public float DefaultLegLengthMeters { get; set; } = 0.86f;

        public float GroundedReleaseLift { get; set; } = 0.035f;
        public float StepOnsetLift { get; set; } = 0.065f;
        public float ChamberKneeLift { get; set; } = 0.145f;
        public float ChamberMaximumKneeAngleDegrees { get; set; } = 146f;
        public int KneeHoldMilliseconds { get; set; } = 180;

        public float KickMinimumAirborneLift { get; set; } = 0.070f;
        public float KickMinimumKneeLift { get; set; } = 0.20f;
        public float KickMinimumKneeAngleDegrees { get; set; } = 150f;
        public float KickMinimumStraightness { get; set; } = 0.86f;
        public float KickMinimumAngleGainDegrees { get; set; } = 20f;
        public float KickMinimumReachGain { get; set; } = 0.070f;
        public float KickMinimumDistalLiftSeparation { get; set; } = 0.14f;
        public float KickMinimumDistalLead { get; set; } = 0.08f;
        public float KickMinimumFrontOcclusionDistalLead { get; set; } = 0.28f;
        public float KickMinimumFrontOcclusionModerateLead { get; set; } = 0.12f;
        public float KickMinimumFrontOcclusionReachGain { get; set; } = 0.20f;
        public float KickMinimumFrontOcclusionLift { get; set; } = 0.18f;
        public int KickMaximumFrontOcclusionMilliseconds { get; set; } = 300;
        public float KickStronglyHeldKneeLift { get; set; } = 0.30f;
        public float KickStronglyHeldMinimumDistalLead { get; set; } = 0.18f;
        public float KickMinimumAngleRateDegreesPerSecond { get; set; } = 65f;
        public float KickMinimumReachRatePerSecond { get; set; } = 0.38f;
        public float KickMinimumDistalDominancePerSecond { get; set; } = 0.08f;
        public float KickMaximumKneeDropRatePerSecond { get; set; } = -0.55f;
        public int KickWindowMilliseconds { get; set; } = 950;
        public int KickMaximumExtensionMilliseconds { get; set; } = 700;
        public int KickRecoveryMilliseconds { get; set; } = 450;
        public int OccludedChamberBridgeMilliseconds { get; set; } = 360;
        public float OccludedChamberMinimumLift { get; set; } = 0.18f;
        public float OccludedKickMinimumLift { get; set; } = 0.24f;
        public float OccludedKickMinimumDistalSeparation { get; set; } = 0.18f;

        public int MinimumStepIntervalMilliseconds { get; set; } = 130;
        public int MaximumStepIntervalMilliseconds { get; set; } = 1250;
        public int AlternationsRequiredForGait { get; set; } = 2;
        public int GaitLockMilliseconds { get; set; } = 900;
        public int RecentGaitKickVetoMilliseconds { get; set; } = 2500;
        public int InvalidFrameGraceMilliseconds { get; set; } = 250;

        // A camera/floor/crop relock can shift both legs by the same amount
        // without changing their anatomy. If both legs remain straight and
        // quiet at that shared offset, treat it as a new neutral origin instead
        // of leaving the classifier permanently airborne/KneeHold.
        public int CommonModeNeutralRebaseMilliseconds { get; set; } = 800;
        public float CommonModeNeutralMinimumResidualMeters { get; set; } = 0.055f;
        public float CommonModeNeutralMaximumResidualDifferenceMeters { get; set; } = 0.080f;
        public float CommonModeNeutralMinimumStraightness { get; set; } = 0.98f;
        public float CommonModeNeutralMaximumJointSpeedPerSecond { get; set; } = 0.12f;

        public float FlipMinimumAnkleAboveKnee { get; set; } = 0.055f;
        public float FlipMaximumFoldedKneeAngleDegrees { get; set; } = 70f;
        public float FlipMaximumInstantJointTravel { get; set; } = 0.42f;
        public float FlipMaximumInstantAngleChangeDegrees { get; set; } = 105f;
    }

    public readonly struct CameraLegClassification
    {
        internal CameraLegClassification(
            CameraLegMotionState state,
            float kneeAngleDegrees,
            float confidence,
            bool isValid,
            bool baselineReady,
            bool anatomicalFlipRejected,
            bool blocksGait,
            bool stepOnset,
            float normalizedAnkleLift,
            float normalizedKneeLift,
            float normalizedReach,
            float extensionRatePerSecond,
            float normalizedDepthDelta,
            float neutralKneeDropNormalized,
            float neutralAnkleDropNormalized,
            float neutralLiftMeters)
        {
            State = state;
            KneeAngleDegrees = kneeAngleDegrees;
            Confidence = confidence;
            IsValid = isValid;
            BaselineReady = baselineReady;
            AnatomicalFlipRejected = anatomicalFlipRejected;
            BlocksGait = blocksGait;
            StepOnset = stepOnset;
            NormalizedAnkleLift = normalizedAnkleLift;
            NormalizedKneeLift = normalizedKneeLift;
            NormalizedReach = normalizedReach;
            ExtensionRatePerSecond = extensionRatePerSecond;
            NormalizedDepthDelta = normalizedDepthDelta;
            NeutralKneeDropNormalized = neutralKneeDropNormalized;
            NeutralAnkleDropNormalized = neutralAnkleDropNormalized;
            NeutralLiftMeters = neutralLiftMeters;
        }

        public CameraLegMotionState State { get; }
        public CameraLegMotionState TransportState => IsValid
            ? State : CameraLegMotionState.Invalid;
        public float KneeAngleDegrees { get; }
        public float Confidence { get; }
        public bool IsValid { get; }
        public bool BaselineReady { get; }
        public bool AnatomicalFlipRejected { get; }
        public bool IsAnatomicalOutlier => AnatomicalFlipRejected;
        public bool BlocksGait { get; }
        public bool StepOnset { get; }

        // Diagnostic features are exposed so a recorded take can be replayed
        // and threshold changes can be measured without opening Skyrim.
        public float NormalizedAnkleLift { get; }
        public float NormalizedKneeLift { get; }
        public float NormalizedReach { get; }
        public float ExtensionRatePerSecond { get; }
        public float NormalizedDepthDelta { get; }
        public float NeutralKneeDropNormalized { get; }
        public float NeutralAnkleDropNormalized { get; }
        public float NeutralLiftMeters { get; }
    }

    public readonly struct CameraLowerBodyClassification
    {
        internal CameraLowerBodyClassification(
            long timestampMs,
            CameraLegClassification left,
            CameraLegClassification right,
            bool coordinatedGait,
            float gaitConfidence)
        {
            TimestampMs = timestampMs;
            Left = left;
            Right = right;
            CoordinatedGait = coordinatedGait;
            GaitConfidence = gaitConfidence;
        }

        public long TimestampMs { get; }
        public CameraLegClassification Left { get; }
        public CameraLegClassification Right { get; }
        public bool CoordinatedGait { get; }
        public float GaitConfidence { get; }
        public bool AnyKick => Left.State == CameraLegMotionState.KickExtend
            || Right.State == CameraLegMotionState.KickExtend;
        public bool SuppressGait => Left.BlocksGait || Right.BlocksGait || AnyKick;
    }

    /// <summary>
    /// Stateful camera-side lower-body classifier. One instance owns the
    /// neutral calibration for one tracking session; call Reset when the
    /// camera/view changes or the main body-coordinate calibration is reset.
    /// </summary>
    public sealed class CameraLowerBodyClassifier
    {
        private sealed class LegTrack
        {
            public CameraLegMotionState State = CameraLegMotionState.Invalid;
            public long StateSinceMs;
            public long LastAcceptedMs = -1;
            public long LastChamberMs = -1;
            public long RecoverBlockUntilMs;
            public bool RecoveringFromKick;

            public bool BaselineReady;
            public int NeutralSamples;
            public float NeutralLegUnits;
            public float NeutralThighUnits;
            public float NeutralShinUnits;
            public float NeutralLegMeters;
            public Vector2 NeutralKneeRelative;
            public Vector2 NeutralAnkleRelative;
            public float NeutralLiftMeters;
            public float NeutralDepthMeters;
            public Vector3 NeutralOutput;
            public bool NeutralOutputValid;

            public Vector2 LastHip;
            public Vector2 LastKnee;
            public Vector2 LastAnkle;
            public Vector3 LastOutput;
            public bool LastOutputValid;
            public float LastKneeAngle = 180f;
            public float LastStraightness = 1f;
            public float LastNormalizedReach = 1f;
            public float LastNormalizedAnkleLift;
            public float LastNormalizedKneeLift;
            public float LastNormalizedDepthDelta;
            public float LastConfidence;

            public bool AirborneLatch;
            public long LastLiftOnsetMs = -1;
            public float ChamberMinimumAngle = 180f;
            public float ChamberMinimumReach = 1f;
            public float ChamberPeakKneeLift;
            public long ChamberStartedMs = -1;
            public long LastOccludedChamberMs = -1;
            public float OccludedChamberMinimumAngle = 180f;
        }

        private readonly struct LegMeasurement
        {
            public LegMeasurement(
                bool valid,
                bool baselineReady,
                bool flipRejected,
                bool stepOnset,
                float jointConfidence,
                float kneeAngle,
                float straightness,
                float ankleLift,
                float kneeLift,
                float normalizedReach,
                float angleRate,
                float reachRate,
                float distalDominance,
                float kneeLiftRate,
                float ankleLiftRate,
                float depthDelta,
                float kneeSpeed,
                float ankleSpeed)
            {
                Valid = valid;
                BaselineReady = baselineReady;
                FlipRejected = flipRejected;
                StepOnset = stepOnset;
                JointConfidence = jointConfidence;
                KneeAngle = kneeAngle;
                Straightness = straightness;
                AnkleLift = ankleLift;
                KneeLift = kneeLift;
                NormalizedReach = normalizedReach;
                AngleRate = angleRate;
                ReachRate = reachRate;
                DistalDominance = distalDominance;
                KneeLiftRate = kneeLiftRate;
                AnkleLiftRate = ankleLiftRate;
                DepthDelta = depthDelta;
                KneeSpeed = kneeSpeed;
                AnkleSpeed = ankleSpeed;
            }

            public bool Valid { get; }
            public bool BaselineReady { get; }
            public bool FlipRejected { get; }
            public bool StepOnset { get; }
            public float JointConfidence { get; }
            public float KneeAngle { get; }
            public float Straightness { get; }
            public float AnkleLift { get; }
            public float KneeLift { get; }
            public float NormalizedReach { get; }
            public float AngleRate { get; }
            public float ReachRate { get; }
            public float DistalDominance { get; }
            public float KneeLiftRate { get; }
            public float AnkleLiftRate { get; }
            public float DepthDelta { get; }
            public float KneeSpeed { get; }
            public float AnkleSpeed { get; }
        }

        private readonly LegTrack[] _legs = { new LegTrack(), new LegTrack() };
        private long _lastTimestampMs = -1;
        private long _lastStepOnsetMs = -1;
        private int _lastStepSide = -1;
        private int _alternationCount;
        private long _gaitLockedUntilMs;
        private long _lastCoordinatedGaitMs = -1;
        private long _commonModeNeutralCandidateSinceMs = -1;

        public CameraLowerBodyClassifier(CameraLowerBodyClassifierOptions? options = null)
        {
            Options = options ?? new CameraLowerBodyClassifierOptions();
        }

        public CameraLowerBodyClassifierOptions Options { get; }

        public void Reset()
        {
            _legs[0] = new LegTrack();
            _legs[1] = new LegTrack();
            _lastTimestampMs = -1;
            _lastStepOnsetMs = -1;
            _lastStepSide = -1;
            _alternationCount = 0;
            _gaitLockedUntilMs = 0;
            _lastCoordinatedGaitMs = -1;
            _commonModeNeutralCandidateSinceMs = -1;
        }

        public CameraLowerBodyClassification Update(in CameraLowerBodyFrame frame)
        {
            long nowMs = frame.TimestampMs;
            if (_lastTimestampMs >= 0 && nowMs <= _lastTimestampMs)
            {
                // Out-of-order camera frames must never rewind state or create
                // an artificial infinite velocity.
                var leftStale = BuildInvalidResult(_legs[0], false);
                var rightStale = BuildInvalidResult(_legs[1], false);
                return new CameraLowerBodyClassification(
                    nowMs, leftStale, rightStale, false, 0f);
            }

            _lastTimestampMs = nowMs;
            LegMeasurement leftMeasurement = MeasureLeg(
                _legs[0], frame.Left, frame.MetersPerImageUnit, nowMs);
            LegMeasurement rightMeasurement = MeasureLeg(
                _legs[1], frame.Right, frame.MetersPerImageUnit, nowMs);

            if (TryRecoverCommonModeNeutral(
                frame, leftMeasurement, rightMeasurement, nowMs))
            {
                // Re-measure against the new neutral on the same camera frame.
                // Positions are already the accepted current sample, so the
                // finite-difference terms are zero and cannot invent a step.
                leftMeasurement = MeasureLeg(
                    _legs[0], frame.Left, frame.MetersPerImageUnit, nowMs);
                rightMeasurement = MeasureLeg(
                    _legs[1], frame.Right, frame.MetersPerImageUnit, nowMs);
            }

            RegisterStepOnsets(leftMeasurement, rightMeasurement, nowMs);
            bool gaitActive = nowMs <= _gaitLockedUntilMs;
            if (gaitActive)
                _lastCoordinatedGaitMs = nowMs;
            bool recentlyCoordinatedGait = _lastCoordinatedGaitMs >= 0
                && nowMs - _lastCoordinatedGaitMs
                    <= Options.RecentGaitKickVetoMilliseconds;

            CameraLegClassification left = AdvanceLeg(
                _legs[0], leftMeasurement, gaitActive,
                WasOppositeLegRecentlyRaised(0, nowMs), recentlyCoordinatedGait, nowMs);
            CameraLegClassification right = AdvanceLeg(
                _legs[1], rightMeasurement, gaitActive,
                WasOppositeLegRecentlyRaised(1, nowMs), recentlyCoordinatedGait, nowMs);

            bool anyKick = left.State == CameraLegMotionState.KickExtend
                || right.State == CameraLegMotionState.KickExtend;
            if (anyKick)
            {
                _gaitLockedUntilMs = 0;
                _alternationCount = 0;
                gaitActive = false;
            }

            float gaitConfidence = gaitActive
                ? Math.Clamp((float)_alternationCount
                    / Math.Max(1, Options.AlternationsRequiredForGait + 1), 0.45f, 1f)
                : 0f;
            return new CameraLowerBodyClassification(
                nowMs, left, right, gaitActive, gaitConfidence);
        }

        private bool TryRecoverCommonModeNeutral(
            in CameraLowerBodyFrame frame,
            in LegMeasurement left,
            in LegMeasurement right,
            long nowMs)
        {
            bool kickActive = _legs[0].State == CameraLegMotionState.KickExtend
                || _legs[1].State == CameraLegMotionState.KickExtend
                || (_legs[0].RecoveringFromKick && nowMs < _legs[0].RecoverBlockUntilMs)
                || (_legs[1].RecoveringFromKick && nowMs < _legs[1].RecoverBlockUntilMs);
            float leftResidual = frame.Left.LiftMeters - _legs[0].NeutralLiftMeters;
            float rightResidual = frame.Right.LiftMeters - _legs[1].NeutralLiftMeters;
            bool finiteResiduals = float.IsFinite(leftResidual) && float.IsFinite(rightResidual);
            bool sharedResidual = finiteResiduals
                && Math.Abs(leftResidual) >= Options.CommonModeNeutralMinimumResidualMeters
                && Math.Abs(rightResidual) >= Options.CommonModeNeutralMinimumResidualMeters
                && Math.Sign(leftResidual) == Math.Sign(rightResidual)
                && Math.Abs(leftResidual - rightResidual)
                    <= Options.CommonModeNeutralMaximumResidualDifferenceMeters;
            float maxJointSpeed = Math.Max(
                Math.Max(left.KneeSpeed, left.AnkleSpeed),
                Math.Max(right.KneeSpeed, right.AnkleSpeed));
            bool stableCandidate = !kickActive
                && left.Valid && right.Valid
                && left.BaselineReady && right.BaselineReady
                && left.Straightness >= Options.CommonModeNeutralMinimumStraightness
                && right.Straightness >= Options.CommonModeNeutralMinimumStraightness
                && maxJointSpeed <= Options.CommonModeNeutralMaximumJointSpeedPerSecond
                && sharedResidual;

            if (!stableCandidate)
            {
                _commonModeNeutralCandidateSinceMs = -1;
                return false;
            }

            if (_commonModeNeutralCandidateSinceMs < 0)
            {
                _commonModeNeutralCandidateSinceMs = nowMs;
                return false;
            }
            if (nowMs - _commonModeNeutralCandidateSinceMs
                < Options.CommonModeNeutralRebaseMilliseconds)
                return false;

            ReanchorNeutral(_legs[0], frame.Left, frame.MetersPerImageUnit, left, nowMs);
            ReanchorNeutral(_legs[1], frame.Right, frame.MetersPerImageUnit, right, nowMs);
            _lastStepOnsetMs = -1;
            _lastStepSide = -1;
            _alternationCount = 0;
            _gaitLockedUntilMs = 0;
            _lastCoordinatedGaitMs = -1;
            _commonModeNeutralCandidateSinceMs = -1;
            return true;
        }

        private void ReanchorNeutral(
            LegTrack track,
            in CameraLegObservation observation,
            float metersPerImageUnit,
            in LegMeasurement measurement,
            long nowMs)
        {
            float thigh = Vector2.Distance(observation.Hip.Position, observation.Knee.Position);
            float shin = Vector2.Distance(observation.Knee.Position, observation.Ankle.Position);
            float legUnits = thigh + shin;
            BlendNeutral(track, observation, thigh, shin, legUnits, metersPerImageUnit, 1f);
            track.NeutralSamples = Math.Max(track.NeutralSamples, Options.NeutralWarmupFrames);
            track.BaselineReady = true;
            track.AirborneLatch = false;
            track.LastLiftOnsetMs = -1;
            track.LastChamberMs = -1;
            track.ChamberMinimumAngle = 180f;
            track.ChamberMinimumReach = 1f;
            track.ChamberPeakKneeLift = 0f;
            track.ChamberStartedMs = -1;
            track.LastOccludedChamberMs = -1;
            track.OccludedChamberMinimumAngle = 180f;
            track.RecoveringFromKick = false;
            track.RecoverBlockUntilMs = 0;
            track.LastNormalizedAnkleLift = 0f;
            track.LastNormalizedKneeLift = 0f;
            track.LastNormalizedDepthDelta = 0f;
            track.LastKneeAngle = measurement.KneeAngle;
            track.LastStraightness = measurement.Straightness;
            track.LastNormalizedReach = measurement.NormalizedReach;
            SetState(track, CameraLegMotionState.Grounded, nowMs);
        }

        private LegMeasurement MeasureLeg(
            LegTrack track,
            in CameraLegObservation observation,
            float metersPerImageUnit,
            long nowMs)
        {
            if (!observation.Hip.IsFinite
                || !observation.Knee.IsFinite
                || !observation.Ankle.IsFinite)
                return InvalidMeasurement(track, false, nowMs);

            float jointConfidence = Math.Clamp(Math.Min(
                observation.Hip.Confidence,
                Math.Min(observation.Knee.Confidence, observation.Ankle.Confidence)), 0f, 1f);
            if (observation.Hip.Confidence < Options.MinimumHipConfidence
                || observation.Knee.Confidence < Options.MinimumLegConfidence
                || observation.Ankle.Confidence < Options.MinimumLegConfidence)
                return InvalidMeasurement(track, false, nowMs);

            Vector2 hip = observation.Hip.Position;
            Vector2 knee = observation.Knee.Position;
            Vector2 ankle = observation.Ankle.Position;
            float thigh = Vector2.Distance(hip, knee);
            float shin = Vector2.Distance(knee, ankle);
            float legUnits = thigh + shin;
            if (!float.IsFinite(legUnits) || thigh < 0.002f || shin < 0.002f || legUnits < 0.01f)
                return InvalidMeasurement(track, false, nowMs);

            float kneeAngle = AngleDegrees(hip - knee, ankle - knee);
            float straightness = Math.Clamp(
                Vector2.Distance(hip, ankle) / Math.Max(0.0001f, legUnits), 0f, 1f);

            bool plausibleNeutral = kneeAngle >= Options.NeutralMinimumKneeAngleDegrees
                && knee.Y > hip.Y - legUnits * 0.04f
                && ankle.Y > knee.Y - legUnits * 0.04f;
            if (!track.BaselineReady && plausibleNeutral)
                AccumulateNeutral(track, observation, thigh, shin, legUnits, metersPerImageUnit);

            float referenceUnits = track.NeutralLegUnits > 0.01f
                ? track.NeutralLegUnits : legUnits;
            float referenceMeters = track.NeutralLegMeters > 0.10f
                ? track.NeutralLegMeters : ResolveLegMeters(legUnits, metersPerImageUnit);
            Vector2 kneeRelative = knee - hip;
            Vector2 ankleRelative = ankle - hip;

            float kneeLift = track.NeutralSamples > 0
                ? (track.NeutralKneeRelative.Y - kneeRelative.Y) / referenceUnits : 0f;
            float imageAnkleLift = track.NeutralSamples > 0
                ? (track.NeutralAnkleRelative.Y - ankleRelative.Y) / referenceUnits : 0f;
            float metricAnkleLift = 0f;
            bool hasMetricLift = track.NeutralSamples > 0
                && float.IsFinite(observation.LiftMeters)
                && float.IsFinite(track.NeutralLiftMeters)
                && referenceMeters > 0.10f;
            if (hasMetricLift)
                metricAnkleLift = (observation.LiftMeters - track.NeutralLiftMeters) / referenceMeters;
            float ankleLift = hasMetricLift
                ? 0.65f * imageAnkleLift + 0.35f * metricAnkleLift
                : imageAnkleLift;

            float normalizedReach = Vector2.Distance(hip, ankle) / referenceUnits;
            float depthDelta = float.IsFinite(observation.DepthMeters) && referenceMeters > 0.10f
                ? (observation.DepthMeters - track.NeutralDepthMeters) / referenceMeters : 0f;

            float dt = track.LastAcceptedMs >= 0
                ? Math.Clamp((nowMs - track.LastAcceptedMs) / 1000f, 0.001f, 0.25f)
                : 1f / 30f;
            float kneeSpeed = track.LastAcceptedMs >= 0
                ? Vector2.Distance(knee, track.LastKnee) / referenceUnits / dt : 0f;
            float ankleSpeed = track.LastAcceptedMs >= 0
                ? Vector2.Distance(ankle, track.LastAnkle) / referenceUnits / dt : 0f;
            float angleRate = track.LastAcceptedMs >= 0
                ? (kneeAngle - track.LastKneeAngle) / dt : 0f;
            float reachRate = track.LastAcceptedMs >= 0
                ? (normalizedReach - track.LastNormalizedReach) / dt : 0f;
            float distalDominance = ankleSpeed - 0.72f * kneeSpeed;
            float kneeLiftRate = track.LastAcceptedMs >= 0
                ? (kneeLift - track.LastNormalizedKneeLift) / dt : 0f;
            float ankleLiftRate = track.LastAcceptedMs >= 0
                ? (ankleLift - track.LastNormalizedAnkleLift) / dt : 0f;

            bool flip = IsAnatomicalFlip(
                track, hip, knee, ankle, thigh, shin, referenceUnits,
                kneeAngle, kneeSpeed, ankleSpeed, nowMs);
            if (flip)
            {
                RememberOccludedChamber(track, kneeAngle, ankleLift, nowMs);
                return InvalidMeasurement(track, true, nowMs);
            }

            bool stepOnset = false;
            bool airborne = ankleLift >= Options.StepOnsetLift
                || kneeLift >= Options.StepOnsetLift * 1.15f;
            bool released = ankleLift <= Options.GroundedReleaseLift
                && kneeLift <= Options.GroundedReleaseLift * 1.45f;
            if (!track.AirborneLatch && airborne)
            {
                track.AirborneLatch = true;
                stepOnset = true;
                track.LastLiftOnsetMs = nowMs;
            }
            else if (track.AirborneLatch && released)
            {
                track.AirborneLatch = false;
            }

            track.LastAcceptedMs = nowMs;
            track.LastHip = hip;
            track.LastKnee = knee;
            track.LastAnkle = ankle;
            track.LastKneeAngle = kneeAngle;
            track.LastStraightness = straightness;
            track.LastNormalizedReach = normalizedReach;
            track.LastNormalizedAnkleLift = ankleLift;
            track.LastNormalizedKneeLift = kneeLift;
            track.LastNormalizedDepthDelta = depthDelta;
            track.LastConfidence = jointConfidence;
            if (observation.OutputFootValid && IsFinite(observation.OutputFootPosition))
            {
                track.LastOutput = observation.OutputFootPosition;
                track.LastOutputValid = true;
            }

            // Only a straight, planted and slow leg may refine its own neutral
            // baseline. This prevents a 45-degree kick from becoming the next
            // frame's definition of "grounded".
            bool stableGround = track.BaselineReady
                && kneeAngle >= Options.NeutralMinimumKneeAngleDegrees
                && Math.Abs(ankleLift) <= Options.GroundedReleaseLift
                && Math.Abs(kneeLift) <= Options.GroundedReleaseLift * 1.45f
                && ankleSpeed < 0.30f;
            if (stableGround)
            {
                AdaptNeutral(track, observation, thigh, shin, legUnits, metersPerImageUnit);
                if (nowMs - track.LastOccludedChamberMs > Options.OccludedChamberBridgeMilliseconds)
                    track.LastOccludedChamberMs = -1;
            }

            return new LegMeasurement(
                track.BaselineReady,
                track.BaselineReady,
                false,
                stepOnset,
                jointConfidence,
                kneeAngle,
                straightness,
                ankleLift,
                kneeLift,
                normalizedReach,
                angleRate,
                reachRate,
                distalDominance,
                kneeLiftRate,
                ankleLiftRate,
                depthDelta,
                kneeSpeed,
                ankleSpeed);
        }

        private void RegisterStepOnsets(
            in LegMeasurement left,
            in LegMeasurement right,
            long nowMs)
        {
            int side = left.Valid && left.StepOnset && !(right.Valid && right.StepOnset)
                ? 0
                : right.Valid && right.StepOnset && !(left.Valid && left.StepOnset) ? 1 : -1;
            if (side < 0)
                return;

            if (_lastStepOnsetMs >= 0)
            {
                long interval = nowMs - _lastStepOnsetMs;
                if (side != _lastStepSide
                    && interval >= Options.MinimumStepIntervalMilliseconds
                    && interval <= Options.MaximumStepIntervalMilliseconds)
                {
                    _alternationCount++;
                    if (_alternationCount >= Options.AlternationsRequiredForGait)
                        _gaitLockedUntilMs = nowMs + Options.GaitLockMilliseconds;
                }
                else if (interval > Options.MaximumStepIntervalMilliseconds || side == _lastStepSide)
                {
                    _alternationCount = 0;
                    _gaitLockedUntilMs = 0;
                }
            }

            _lastStepOnsetMs = nowMs;
            _lastStepSide = side;
        }

        private CameraLegClassification AdvanceLeg(
            LegTrack track,
            in LegMeasurement measurement,
            bool gaitActive,
            bool oppositeLegRecentlyRaised,
            bool recentlyCoordinatedGait,
            long nowMs)
        {
            if (!measurement.Valid)
                return BuildInvalidResult(track, measurement.FlipRejected);

            bool grounded = measurement.AnkleLift <= Options.GroundedReleaseLift
                && measurement.KneeLift <= Options.GroundedReleaseLift * 1.45f;
            bool airborne = measurement.AnkleLift >= Options.StepOnsetLift
                || measurement.KneeLift >= Options.StepOnsetLift * 1.15f;
            bool chamberPose = airborne
                && measurement.KneeLift >= Options.ChamberKneeLift
                && measurement.KneeAngle <= Options.ChamberMaximumKneeAngleDegrees;

            if (!gaitActive
                && !oppositeLegRecentlyRaised
                && !recentlyCoordinatedGait
                && IsOcclusionBridgedKick(track, measurement, nowMs))
            {
                SetState(track, CameraLegMotionState.KickExtend, nowMs);
                track.RecoveringFromKick = true;
                track.RecoverBlockUntilMs = nowMs + Options.KickRecoveryMilliseconds;
                track.LastOccludedChamberMs = -1;
            }
            else if (gaitActive && airborne && track.State != CameraLegMotionState.KickExtend)
            {
                SetState(track, CameraLegMotionState.WalkStep, nowMs);
                track.RecoveringFromKick = false;
            }
            else
            {
                switch (track.State)
                {
                    case CameraLegMotionState.Grounded:
                        if (airborne)
                        {
                            if (chamberPose)
                                BeginChamber(track, measurement, nowMs);
                            else
                                SetState(track, CameraLegMotionState.UndecidedLift, nowMs);
                        }
                        break;

                    case CameraLegMotionState.Invalid:
                    case CameraLegMotionState.UndecidedLift:
                        if (grounded)
                        {
                            SetState(track, CameraLegMotionState.Grounded, nowMs);
                        }
                        else if (chamberPose)
                        {
                            BeginChamber(track, measurement, nowMs);
                        }
                        break;

                    case CameraLegMotionState.Chamber:
                    case CameraLegMotionState.KneeHold:
                        UpdateChamberExtrema(track, measurement, nowMs);
                        if (!oppositeLegRecentlyRaised
                            && !recentlyCoordinatedGait
                            && IsKickExtension(track, measurement, nowMs))
                        {
                            SetState(track, CameraLegMotionState.KickExtend, nowMs);
                            track.RecoveringFromKick = true;
                            track.RecoverBlockUntilMs = nowMs + Options.KickRecoveryMilliseconds;
                        }
                        else if (grounded)
                        {
                            SetState(track, CameraLegMotionState.Recover, nowMs);
                            track.RecoveringFromKick = false;
                        }
                        else if (track.State == CameraLegMotionState.Chamber
                            && nowMs - track.StateSinceMs >= Options.KneeHoldMilliseconds)
                        {
                            SetState(track, CameraLegMotionState.KneeHold, nowMs);
                        }
                        break;

                    case CameraLegMotionState.KickExtend:
                        if (grounded
                            || measurement.KneeAngle < Options.KickMinimumKneeAngleDegrees - 12f
                            || measurement.ReachRate < -0.28f
                            || nowMs - track.StateSinceMs >= Options.KickMaximumExtensionMilliseconds)
                        {
                            SetState(track, CameraLegMotionState.Recover, nowMs);
                            track.RecoveringFromKick = true;
                            track.RecoverBlockUntilMs = nowMs + Options.KickRecoveryMilliseconds;
                        }
                        break;

                    case CameraLegMotionState.Recover:
                        if (grounded && nowMs - track.StateSinceMs >= 120)
                        {
                            SetState(track, CameraLegMotionState.Grounded, nowMs);
                            if (nowMs >= track.RecoverBlockUntilMs)
                                track.RecoveringFromKick = false;
                        }
                        else if (chamberPose && nowMs >= track.RecoverBlockUntilMs)
                        {
                            BeginChamber(track, measurement, nowMs);
                        }
                        break;

                    case CameraLegMotionState.WalkStep:
                        if (grounded)
                        {
                            SetState(track, CameraLegMotionState.Grounded, nowMs);
                        }
                        else if (!gaitActive && chamberPose)
                        {
                            BeginChamber(track, measurement, nowMs);
                        }
                        else if (!gaitActive && !airborne)
                        {
                            SetState(track, CameraLegMotionState.UndecidedLift, nowMs);
                        }
                        break;
                }
            }

            bool blocksGait = track.State == CameraLegMotionState.Chamber
                || track.State == CameraLegMotionState.KneeHold
                || track.State == CameraLegMotionState.KickExtend
                || (track.State == CameraLegMotionState.Recover
                    && track.RecoveringFromKick
                    && nowMs < track.RecoverBlockUntilMs);
            float confidence = StateConfidence(track.State, measurement, gaitActive)
                * measurement.JointConfidence;

            return new CameraLegClassification(
                track.State,
                measurement.KneeAngle,
                Math.Clamp(confidence, 0f, 1f),
                true,
                measurement.BaselineReady,
                false,
                blocksGait,
                measurement.StepOnset,
                measurement.AnkleLift,
                measurement.KneeLift,
                measurement.NormalizedReach,
                measurement.ReachRate,
                measurement.DepthDelta,
                NeutralDrop(track.NeutralKneeRelative.Y, track.NeutralLegUnits),
                NeutralDrop(track.NeutralAnkleRelative.Y, track.NeutralLegUnits),
                track.NeutralLiftMeters);
        }

        private bool IsKickExtension(LegTrack track, in LegMeasurement m, long nowMs)
        {
            if (track.LastChamberMs < 0 || nowMs - track.LastChamberMs > Options.KickWindowMilliseconds)
                return false;

            float angleGain = m.KneeAngle - track.ChamberMinimumAngle;
            float reachGain = m.NormalizedReach - track.ChamberMinimumReach;
            bool extensionImpulse = m.AngleRate >= Options.KickMinimumAngleRateDegreesPerSecond
                || m.ReachRate >= Options.KickMinimumReachRatePerSecond;
            // At +/-45 degrees a kick is often foreshortened: image-space
            // hip-to-ankle reach can shrink while the knee clearly straightens.
            // In that view the distal ankle rising away from the held knee is
            // the complementary extension signal. A plain knee lift keeps the
            // knee and ankle moving together and fails both alternatives.
            bool extensionShape = reachGain >= Options.KickMinimumReachGain
                || m.AnkleLift - m.KneeLift >= Options.KickMinimumDistalLiftSeparation;
            bool kneeHeldInProjection = m.KneeLift >= Options.KickMinimumKneeLift;
            float distalLead = m.AnkleLift - m.KneeLift;
            bool frontOcclusionProjection = track.ChamberPeakKneeLift
                    >= Options.KickMinimumKneeLift
                && track.ChamberStartedMs >= 0
                && nowMs - track.ChamberStartedMs
                    <= Options.KickMaximumFrontOcclusionMilliseconds
                && (distalLead >= Options.KickMinimumFrontOcclusionDistalLead
                    || (distalLead >= Options.KickMinimumFrontOcclusionModerateLead
                        && reachGain >= Options.KickMinimumFrontOcclusionReachGain));
            frontOcclusionProjection = frontOcclusionProjection
                && m.AnkleLift >= Options.KickMinimumFrontOcclusionLift;
            bool strongHeldApex = m.KneeLift >= Options.KickStronglyHeldKneeLift
                && (distalLead >= Options.KickStronglyHeldMinimumDistalLead
                    || m.KneeAngle >= 168f);
            bool kneeMotionValid = strongHeldApex
                || frontOcclusionProjection
                || m.KneeLiftRate >= Options.KickMaximumKneeDropRatePerSecond;
            return m.AnkleLift >= Options.KickMinimumAirborneLift
                && (kneeHeldInProjection || frontOcclusionProjection)
                && m.KneeAngle >= Options.KickMinimumKneeAngleDegrees
                && m.Straightness >= Options.KickMinimumStraightness
                && angleGain >= Options.KickMinimumAngleGainDegrees
                && m.AnkleLift - m.KneeLift >= Options.KickMinimumDistalLead
                && extensionShape
                && extensionImpulse
                && kneeMotionValid
                && (frontOcclusionProjection
                    || m.DistalDominance >= Options.KickMinimumDistalDominancePerSecond);
        }

        private bool WasOppositeLegRecentlyRaised(int side, long nowMs)
        {
            long oppositeOnset = _legs[side ^ 1].LastLiftOnsetMs;
            return oppositeOnset >= 0
                && nowMs - oppositeOnset <= Options.MaximumStepIntervalMilliseconds;
        }

        private bool IsOcclusionBridgedKick(
            LegTrack track,
            in LegMeasurement measurement,
            long nowMs)
        {
            if (track.LastOccludedChamberMs < 0
                || nowMs - track.LastOccludedChamberMs > Options.OccludedChamberBridgeMilliseconds)
                return false;

            float angleGain = measurement.KneeAngle - track.OccludedChamberMinimumAngle;
            return measurement.KneeAngle >= Options.KickMinimumKneeAngleDegrees
                && measurement.Straightness >= Options.KickMinimumStraightness
                && measurement.AnkleLift >= Options.OccludedKickMinimumLift
                && measurement.AnkleLift - measurement.KneeLift
                    >= Options.OccludedKickMinimumDistalSeparation
                && angleGain >= Options.KickMinimumAngleGainDegrees
                && measurement.AnkleLiftRate > 0.10f
                && measurement.DistalDominance
                    >= Options.KickMinimumDistalDominancePerSecond;
        }

        private void RememberOccludedChamber(
            LegTrack track,
            float kneeAngle,
            float normalizedAnkleLift,
            long nowMs)
        {
            if (!track.BaselineReady
                || normalizedAnkleLift < Options.OccludedChamberMinimumLift
                || kneeAngle >= Options.ChamberMaximumKneeAngleDegrees)
                return;

            if (track.LastOccludedChamberMs < 0
                || nowMs - track.LastOccludedChamberMs > Options.OccludedChamberBridgeMilliseconds)
                track.OccludedChamberMinimumAngle = kneeAngle;
            else
                track.OccludedChamberMinimumAngle = Math.Min(
                    track.OccludedChamberMinimumAngle, kneeAngle);
            track.LastOccludedChamberMs = nowMs;
        }

        private static float StateConfidence(
            CameraLegMotionState state,
            in LegMeasurement measurement,
            bool gaitActive)
        {
            return state switch
            {
                CameraLegMotionState.Grounded => 0.92f,
                CameraLegMotionState.Invalid => 0f,
                CameraLegMotionState.UndecidedLift => 0.38f,
                CameraLegMotionState.Chamber => 0.70f,
                CameraLegMotionState.KneeHold => 0.86f,
                CameraLegMotionState.KickExtend => Math.Clamp(
                    0.58f + Math.Max(0f, measurement.Straightness - 0.82f) * 2f, 0.58f, 0.98f),
                CameraLegMotionState.Recover => 0.70f,
                CameraLegMotionState.WalkStep => gaitActive ? 0.90f : 0.55f,
                _ => 0.35f,
            };
        }

        private bool IsAnatomicalFlip(
            LegTrack track,
            Vector2 hip,
            Vector2 knee,
            Vector2 ankle,
            float thigh,
            float shin,
            float referenceUnits,
            float kneeAngle,
            float kneeSpeed,
            float ankleSpeed,
            long nowMs)
        {
            float ankleAboveKnee = (knee.Y - ankle.Y) / referenceUnits;
            if (ankleAboveKnee >= Options.FlipMinimumAnkleAboveKnee
                && kneeAngle <= Options.FlipMaximumFoldedKneeAngleDegrees)
                return true;

            // Perspective can make either projected segment nearly vanish or
            // grow sharply at +/-45 degrees. Segment-length ratios therefore
            // affect confidence but are not an anatomical rejection by
            // themselves; topology and the temporal swap tests are invariant
            // to that foreshortening.

            if (track.LastAcceptedMs >= 0 && nowMs - track.LastAcceptedMs <= 100)
            {
                float angleJump = Math.Abs(kneeAngle - track.LastKneeAngle);
                if (angleJump >= Options.FlipMaximumInstantAngleChangeDegrees
                    && kneeAngle < Options.KickMinimumKneeAngleDegrees
                    && ankleSpeed >= Options.FlipMaximumInstantJointTravel)
                    return true;

                float normalCost = Vector2.Distance(knee, track.LastKnee)
                    + Vector2.Distance(ankle, track.LastAnkle);
                float swappedCost = Vector2.Distance(knee, track.LastAnkle)
                    + Vector2.Distance(ankle, track.LastKnee);
                if (swappedCost + referenceUnits * 0.08f < normalCost
                    && kneeAngle < 95f
                    && Math.Max(kneeSpeed, ankleSpeed) > 0.50f)
                    return true;
            }

            // A knee above the hip is possible, but not while both segments
            // fold back through the torso in one low-angle frame.
            if ((hip.Y - knee.Y) / referenceUnits > 0.18f && kneeAngle < 55f)
                return true;
            return false;
        }

        private void AccumulateNeutral(
            LegTrack track,
            in CameraLegObservation observation,
            float thigh,
            float shin,
            float legUnits,
            float metersPerImageUnit)
        {
            float alpha = track.NeutralSamples == 0
                ? 1f : 1f / Math.Min(track.NeutralSamples + 1, Options.NeutralWarmupFrames);
            BlendNeutral(track, observation, thigh, shin, legUnits, metersPerImageUnit, alpha);
            track.NeutralSamples++;
            if (track.NeutralSamples >= Math.Max(1, Options.NeutralWarmupFrames))
            {
                track.BaselineReady = true;
                track.State = CameraLegMotionState.Grounded;
            }
        }

        private void AdaptNeutral(
            LegTrack track,
            in CameraLegObservation observation,
            float thigh,
            float shin,
            float legUnits,
            float metersPerImageUnit)
        {
            BlendNeutral(track, observation, thigh, shin, legUnits, metersPerImageUnit,
                Math.Clamp(Options.NeutralAdaptation, 0.001f, 0.25f));
        }

        private void BlendNeutral(
            LegTrack track,
            in CameraLegObservation observation,
            float thigh,
            float shin,
            float legUnits,
            float metersPerImageUnit,
            float alpha)
        {
            Vector2 kneeRelative = observation.Knee.Position - observation.Hip.Position;
            Vector2 ankleRelative = observation.Ankle.Position - observation.Hip.Position;
            float legMeters = ResolveLegMeters(legUnits, metersPerImageUnit);
            track.NeutralLegUnits = Lerp(track.NeutralLegUnits, legUnits, alpha);
            track.NeutralThighUnits = Lerp(track.NeutralThighUnits, thigh, alpha);
            track.NeutralShinUnits = Lerp(track.NeutralShinUnits, shin, alpha);
            track.NeutralLegMeters = Lerp(track.NeutralLegMeters, legMeters, alpha);
            track.NeutralKneeRelative = Vector2.Lerp(track.NeutralKneeRelative, kneeRelative, alpha);
            track.NeutralAnkleRelative = Vector2.Lerp(track.NeutralAnkleRelative, ankleRelative, alpha);
            if (float.IsFinite(observation.LiftMeters))
                track.NeutralLiftMeters = Lerp(track.NeutralLiftMeters, observation.LiftMeters, alpha);
            if (float.IsFinite(observation.DepthMeters))
                track.NeutralDepthMeters = Lerp(track.NeutralDepthMeters, observation.DepthMeters, alpha);
            if (observation.OutputFootValid && IsFinite(observation.OutputFootPosition))
            {
                track.NeutralOutput = track.NeutralOutputValid
                    ? Vector3.Lerp(track.NeutralOutput, observation.OutputFootPosition, alpha)
                    : observation.OutputFootPosition;
                track.NeutralOutputValid = true;
            }
        }

        private float ResolveLegMeters(float legUnits, float metersPerImageUnit)
        {
            float measured = legUnits * metersPerImageUnit;
            return float.IsFinite(measured) && measured >= 0.35f && measured <= 1.30f
                ? measured : Options.DefaultLegLengthMeters;
        }

        private LegMeasurement InvalidMeasurement(LegTrack track, bool flip, long nowMs)
        {
            if (track.LastAcceptedMs >= 0
                && nowMs - track.LastAcceptedMs > Options.InvalidFrameGraceMilliseconds)
            {
                SetState(track, CameraLegMotionState.UndecidedLift, nowMs);
                track.AirborneLatch = false;
            }
            return new LegMeasurement(
                false,
                track.BaselineReady,
                flip,
                false,
                0f,
                track.LastKneeAngle,
                track.LastStraightness,
                track.LastNormalizedAnkleLift,
                track.LastNormalizedKneeLift,
                track.LastNormalizedReach,
                0f,
                0f,
                0f,
                0f,
                0f,
                track.LastNormalizedDepthDelta,
                0f,
                0f);
        }

        private static CameraLegClassification BuildInvalidResult(LegTrack track, bool flip)
        {
            return new CameraLegClassification(
                track.State,
                track.LastKneeAngle,
                0f,
                false,
                track.BaselineReady,
                flip,
                track.State == CameraLegMotionState.KickExtend
                    || track.State == CameraLegMotionState.Chamber
                    || track.State == CameraLegMotionState.KneeHold,
                false,
                track.LastNormalizedAnkleLift,
                track.LastNormalizedKneeLift,
                track.LastNormalizedReach,
                0f,
                track.LastNormalizedDepthDelta,
                NeutralDrop(track.NeutralKneeRelative.Y, track.NeutralLegUnits),
                NeutralDrop(track.NeutralAnkleRelative.Y, track.NeutralLegUnits),
                track.NeutralLiftMeters);
        }

        private static void BeginChamber(LegTrack track, in LegMeasurement measurement, long nowMs)
        {
            SetState(track, CameraLegMotionState.Chamber, nowMs);
            track.LastChamberMs = nowMs;
            track.ChamberMinimumAngle = measurement.KneeAngle;
            track.ChamberMinimumReach = measurement.NormalizedReach;
            track.ChamberPeakKneeLift = measurement.KneeLift;
            track.ChamberStartedMs = nowMs;
        }

        private static void UpdateChamberExtrema(
            LegTrack track,
            in LegMeasurement measurement,
            long nowMs)
        {
            if (measurement.KneeAngle <= 150f)
            {
                track.LastChamberMs = nowMs;
                track.ChamberMinimumAngle = Math.Min(track.ChamberMinimumAngle, measurement.KneeAngle);
                track.ChamberMinimumReach = Math.Min(track.ChamberMinimumReach, measurement.NormalizedReach);
                track.ChamberPeakKneeLift = Math.Max(track.ChamberPeakKneeLift, measurement.KneeLift);
            }
        }

        private static void SetState(LegTrack track, CameraLegMotionState state, long nowMs)
        {
            if (track.State == state)
                return;
            track.State = state;
            track.StateSinceMs = nowMs;
        }

        private static float AngleDegrees(Vector2 a, Vector2 b)
        {
            float denom = a.Length() * b.Length();
            if (denom < 0.000001f)
                return 0f;
            float cosine = Math.Clamp(Vector2.Dot(a, b) / denom, -1f, 1f);
            return MathF.Acos(cosine) * (180f / MathF.PI);
        }

        private static float Lerp(float current, float target, float alpha) =>
            current + alpha * (target - current);

        private static float NeutralDrop(float relativeY, float legUnits) =>
            legUnits > 0.001f ? relativeY / legUnits : 0f;

        private static bool IsFinite(Vector3 value) =>
            float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);
    }

    /// <summary>
    /// Converts the camera solver's inferred 3D shin direction into the
    /// body-local foot orientation used during a confirmed kick. Heel/toe
    /// landmarks are intentionally not inputs: those are least reliable when
    /// a foot points toward a monocular camera.
    /// </summary>
    internal static class CameraFootOrientationSolver
    {
        internal static bool TrySolveKickEuler(
            Vector3 kneePosition,
            Vector3 footPosition,
            out Vector3 eulerDegrees)
        {
            eulerDegrees = Vector3.Zero;
            if (!IsFinite(kneePosition) || !IsFinite(footPosition))
                return false;

            Vector3 shin = footPosition - kneePosition;
            if (shin.LengthSquared() < 0.0064f) // 8 cm: reject a collapsed/noisy solve
                return false;

            // Camera depth is unsigned body-forward depth. Never let a small
            // recovery-frame depth wobble turn the foot 180 degrees backward.
            float forward = Math.Max(0.04f, shin.Z);
            float horizontal = MathF.Sqrt(shin.X * shin.X + forward * forward);
            const float radToDeg = 180f / MathF.PI;
            float pitch = Math.Clamp(
                -MathF.Atan2(shin.Y, horizontal) * radToDeg, -35f, 55f);
            float yaw = Math.Clamp(
                MathF.Atan2(shin.X, forward) * radToDeg, -55f, 55f);
            eulerDegrees = new Vector3(pitch, yaw, 0f);
            return IsFinite(eulerDegrees);
        }

        private static bool IsFinite(Vector3 value) =>
            float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);
    }

    /// <summary>Deterministic offline replay for CSV adapters and tests.</summary>
    public static class CameraLowerBodyClassifierReplay
    {
        public static IReadOnlyList<CameraLowerBodyClassification> Run(
            IEnumerable<CameraLowerBodyFrame> frames,
            CameraLowerBodyClassifierOptions? options = null)
        {
            ArgumentNullException.ThrowIfNull(frames);
            var classifier = new CameraLowerBodyClassifier(options);
            var output = new List<CameraLowerBodyClassification>();
            foreach (CameraLowerBodyFrame frame in frames)
                output.Add(classifier.Update(frame));
            return output;
        }
    }

    public sealed class CameraLowerBodyClassifierSelfTestResult
    {
        internal CameraLowerBodyClassifierSelfTestResult(List<string> failures)
        {
            Failures = failures.AsReadOnly();
        }

        public IReadOnlyList<string> Failures { get; }
        public bool Passed => Failures.Count == 0;
        public override string ToString() => Passed
            ? "Camera lower-body classifier self-test passed."
            : string.Join(Environment.NewLine, Failures);
    }

    /// <summary>
    /// Framework-free deterministic smoke tests. This remains callable from a
    /// PowerShell-loaded build without adding a test package or csproj entry.
    /// </summary>
    public static class CameraLowerBodyClassifierSelfTest
    {
        public static CameraLowerBodyClassifierSelfTestResult Run()
        {
            var failures = new List<string>();
            TestKneeHoldAndKick(failures);
            TestFlipRejection(failures);
            TestOccludedChamberBridge(failures);
            TestAlternatingWalk(failures);
            TestHighMarchDoesNotKick(failures);
            TestCommonModeNeutralRebase(failures);
            TestFootOrientationSymmetry(failures);
            TestLegacy2DTrackerGeometry(failures);
            return new CameraLowerBodyClassifierSelfTestResult(failures);
        }

        private static void TestKneeHoldAndKick(List<string> failures)
        {
            var classifier = new CameraLowerBodyClassifier();
            long t = WarmNeutral(classifier, 0);
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < 9; i++)
            {
                result = classifier.Update(Frame(t += 33, ChamberLeg(0), NeutralLeg(1)));
            }
            if (result.Left.State != CameraLegMotionState.KneeHold)
                failures.Add($"Bent knee hold became {result.Left.State}, expected KneeHold.");
            if (result.AnyKick)
                failures.Add("A stationary bent knee was classified as a kick.");

            classifier.Reset();
            t = WarmNeutral(classifier, 0);
            for (int i = 0; i < 5; i++)
                classifier.Update(Frame(t += 33, ChamberLeg(0), NeutralLeg(1)));
            result = default;
            for (int i = 0; i < 5; i++)
            {
                float amount = (i + 1) / 5f;
                result = classifier.Update(Frame(t += 33, ExtendingLeg(0, amount), NeutralLeg(1)));
            }
            if (result.Left.State != CameraLegMotionState.KickExtend)
                failures.Add($"Chamber-to-extension became {result.Left.State}, expected KickExtend.");
        }

        private static void TestFlipRejection(List<string> failures)
        {
            var classifier = new CameraLowerBodyClassifier();
            long t = WarmNeutral(classifier, 0);
            CameraLegObservation flipped = MakeLeg(0,
                new Vector2(0.45f, 0.35f),
                new Vector2(0.45f, 0.63f),
                new Vector2(0.45f, 0.48f), 0.30f, 0.20f);
            CameraLowerBodyClassification result = classifier.Update(
                Frame(t + 33, flipped, NeutralLeg(1)));
            if (!result.Left.AnatomicalFlipRejected || result.Left.IsValid)
                failures.Add("An inverted knee/ankle frame was not rejected.");
        }

        private static void TestAlternatingWalk(List<string> failures)
        {
            var classifier = new CameraLowerBodyClassifier();
            long t = WarmNeutral(classifier, 0);
            CameraLowerBodyClassification result = default;
            for (int step = 0; step < 4; step++)
            {
                int raised = step & 1;
                for (int i = 0; i < 3; i++)
                {
                    result = classifier.Update(Frame(t += 50,
                        raised == 0 ? StepLeg(0) : NeutralLeg(0),
                        raised == 1 ? StepLeg(1) : NeutralLeg(1)));
                }
                for (int i = 0; i < 3; i++)
                    result = classifier.Update(Frame(t += 50, NeutralLeg(0), NeutralLeg(1)));
            }
            if (!result.CoordinatedGait)
                failures.Add("Alternating left/right steps did not establish coordinated gait.");
        }

        private static void TestOccludedChamberBridge(List<string> failures)
        {
            var classifier = new CameraLowerBodyClassifier();
            long t = WarmNeutral(classifier, 0);
            CameraLegObservation folded = MakeLeg(0,
                new Vector2(0.45f, 0.35f),
                new Vector2(0.45f, 0.63f),
                new Vector2(0.45f, 0.45f), 0.55f, 0.45f);
            CameraLowerBodyClassification rejected = classifier.Update(
                Frame(t += 33, folded, NeutralLeg(1)));
            CameraLegObservation relockedExtension = MakeLeg(0,
                new Vector2(0.45f, 0.35f),
                new Vector2(0.45f, 0.60f),
                new Vector2(0.45f, 0.68f), 0.38f, 0.50f);
            CameraLowerBodyClassification extension = classifier.Update(
                Frame(t + 33, relockedExtension, NeutralLeg(1)));
            if (!rejected.Left.AnatomicalFlipRejected)
                failures.Add("Synthetic self-occluded chamber was not rejected as an outlier.");
            if (extension.Left.State != CameraLegMotionState.KickExtend)
                failures.Add("A rejected chamber followed by a valid straight extension was lost.");
        }

        private static void TestHighMarchDoesNotKick(List<string> failures)
        {
            var classifier = new CameraLowerBodyClassifier();
            long t = WarmNeutral(classifier, 0);
            bool kicked = false;
            for (int step = 0; step < 4; step++)
            {
                int raised = step & 1;
                for (int i = 0; i < 8; i++)
                {
                    CameraLowerBodyClassification result = classifier.Update(Frame(t += 40,
                        raised == 0 ? ChamberLeg(0) : NeutralLeg(0),
                        raised == 1 ? ChamberLeg(1) : NeutralLeg(1)));
                    kicked |= result.AnyKick;
                }
                for (int i = 0; i < 4; i++)
                    classifier.Update(Frame(t += 40, NeutralLeg(0), NeutralLeg(1)));
            }
            if (kicked)
                failures.Add("Alternating high marches were classified as kicks.");
        }

        private static void TestCommonModeNeutralRebase(List<string> failures)
        {
            var classifier = new CameraLowerBodyClassifier();
            long t = WarmNeutral(classifier, 0);
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < 24; i++)
            {
                result = classifier.Update(Frame(t += 50,
                    ShiftedNeutralLeg(0), ShiftedNeutralLeg(1)));
            }
            if (result.Left.State != CameraLegMotionState.Grounded
                || result.Right.State != CameraLegMotionState.Grounded
                || Math.Abs(result.Left.NeutralLiftMeters - 0.18f) > 0.02f
                || Math.Abs(result.Right.NeutralLiftMeters - 0.18f) > 0.02f)
            {
                failures.Add("A stable common-mode camera/floor shift did not re-anchor both neutral legs.");
            }
        }

        private static void TestFootOrientationSymmetry(List<string> failures)
        {
            var knee = new Vector3(0f, 0.52f, 0.20f);
            bool leftValid = CameraFootOrientationSolver.TrySolveKickEuler(
                knee, new Vector3(-0.24f, 0.31f, 0.72f), out Vector3 left);
            bool rightValid = CameraFootOrientationSolver.TrySolveKickEuler(
                knee, new Vector3(0.24f, 0.31f, 0.72f), out Vector3 right);
            if (!leftValid || !rightValid)
            {
                failures.Add("A valid inferred kick did not produce a foot orientation.");
                return;
            }

            if (Math.Abs(left.X - right.X) > 0.001f
                || Math.Abs(left.Y + right.Y) > 0.001f
                || left.Z != 0f || right.Z != 0f)
            {
                failures.Add("Mirrored kick geometry did not produce symmetric foot orientation.");
            }

            if (CameraFootOrientationSolver.TrySolveKickEuler(
                knee, knee + new Vector3(0.01f, 0.01f, 0.01f), out _))
            {
                failures.Add("Collapsed inferred kick geometry produced a foot orientation.");
            }
        }

        private static void TestLegacy2DTrackerGeometry(List<string> failures)
        {
            float aspect = Legacy2DTrackerGeometry.ResolveImageAspect(1920, 1080);
            float left = Legacy2DTrackerGeometry.MetricX(0.40f, 1f, aspect);
            float right = Legacy2DTrackerGeometry.MetricX(0.60f, 1f, aspect);
            float expectedSeparation = 0.20f * (1920f / 1080f);
            if (MathF.Abs((left - right) - expectedSeparation) > 0.00001f)
            {
                failures.Add("Legacy 2D X conversion did not restore the camera aspect ratio.");
            }

            float correctedLeft = Legacy2DTrackerGeometry.ToIsotropicImageX(0.40f, aspect);
            float correctedRight = Legacy2DTrackerGeometry.ToIsotropicImageX(0.60f, aspect);
            if (MathF.Abs((correctedRight - correctedLeft) - expectedSeparation) > 0.00001f)
            {
                failures.Add("Legacy classifier X coordinates were not aspect-corrected.");
            }

            float normalizedDelta = Legacy2DTrackerGeometry.MetersToImageDelta(
                0.25f, 2f, aspect);
            float roundTrip = Legacy2DTrackerGeometry.ImageDeltaToMeters(
                normalizedDelta, 2f, aspect);
            if (MathF.Abs(roundTrip - 0.25f) > 0.00001f)
            {
                failures.Add("Legacy lateral clamp conversion did not round-trip through aspect correction.");
            }

            float kickAllowance = Legacy2DTrackerGeometry.MetersToImageDelta(
                0.42f, 2f, aspect);
            float emittedKickAllowance = Legacy2DTrackerGeometry.ImageDeltaToMeters(
                kickAllowance, 2f, aspect);
            if (MathF.Abs(emittedKickAllowance - 0.42f) > 0.00001f)
            {
                failures.Add("Legacy kick clamp did not preserve its 42 cm body-space allowance.");
            }

            float groundedKnee = 0.18f;
            float groundedFoot = 0.42f;
            Legacy2DTrackerGeometry.PinGroundedDepth(
                true, ref groundedKnee, ref groundedFoot);
            if (groundedKnee != 0f || groundedFoot != 0f)
                failures.Add("A grounded legacy foot retained divergent inferred depth.");

            float raisedKnee = 0.18f;
            float raisedFoot = 0.42f;
            Legacy2DTrackerGeometry.PinGroundedDepth(
                false, ref raisedKnee, ref raisedFoot);
            if (raisedKnee != 0.18f || raisedFoot != 0.42f)
                failures.Add("Raised/kicking legacy depth was flattened with grounded depth.");
        }

        private static long WarmNeutral(CameraLowerBodyClassifier classifier, long start)
        {
            long t = start;
            for (int i = 0; i < 12; i++)
                classifier.Update(Frame(t += 33, NeutralLeg(0), NeutralLeg(1)));
            return t;
        }

        private static CameraLowerBodyFrame Frame(
            long timestamp,
            CameraLegObservation left,
            CameraLegObservation right) =>
            new CameraLowerBodyFrame(timestamp, 1.72f, left, right);

        private static CameraLegObservation NeutralLeg(int side)
        {
            float x = side == 0 ? 0.45f : 0.55f;
            return MakeLeg(side,
                new Vector2(x, 0.35f),
                new Vector2(x, 0.60f),
                new Vector2(x, 0.85f), 0.08f, 0.02f);
        }

        private static CameraLegObservation StepLeg(int side)
        {
            float x = side == 0 ? 0.45f : 0.55f;
            return MakeLeg(side,
                new Vector2(x, 0.35f),
                new Vector2(x + (side == 0 ? -0.01f : 0.01f), 0.58f),
                new Vector2(x + (side == 0 ? -0.015f : 0.015f), 0.80f), 0.165f, 0.04f);
        }

        private static CameraLegObservation ShiftedNeutralLeg(int side)
        {
            float x = side == 0 ? 0.45f : 0.55f;
            return MakeLeg(side,
                new Vector2(x, 0.35f),
                new Vector2(x, 0.60f),
                new Vector2(x, 0.85f), 0.18f, 0.02f);
        }

        private static CameraLegObservation ChamberLeg(int side)
        {
            float x = side == 0 ? 0.45f : 0.55f;
            float direction = side == 0 ? 1f : -1f;
            return MakeLeg(side,
                new Vector2(x, 0.35f),
                new Vector2(x, 0.48f),
                new Vector2(x + direction * 0.13f, 0.50f), 0.40f, 0.12f);
        }

        private static CameraLegObservation ExtendingLeg(int side, float amount)
        {
            float x = side == 0 ? 0.45f : 0.55f;
            float direction = side == 0 ? 1f : -1f;
            Vector2 chamberAnkle = new Vector2(x + direction * 0.13f, 0.50f);
            Vector2 extendedAnkle = new Vector2(x + direction * 0.015f, 0.75f);
            return MakeLeg(side,
                new Vector2(x, 0.35f),
                new Vector2(x, 0.48f),
                Vector2.Lerp(chamberAnkle, extendedAnkle, amount), 0.28f, 0.45f);
        }

        private static CameraLegObservation MakeLeg(
            int side,
            Vector2 hip,
            Vector2 knee,
            Vector2 ankle,
            float lift,
            float depth)
        {
            float outputX = side == 0 ? -0.10f : 0.10f;
            return new CameraLegObservation(
                new CameraJoint2D(hip.X, hip.Y, 0.95f),
                new CameraJoint2D(knee.X, knee.Y, 0.90f),
                new CameraJoint2D(ankle.X, ankle.Y, 0.85f),
                lift,
                depth,
                true,
                new Vector3(outputX, Math.Max(0f, lift - 0.08f), depth));
        }
    }
}
