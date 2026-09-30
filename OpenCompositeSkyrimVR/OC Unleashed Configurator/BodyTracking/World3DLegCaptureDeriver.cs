using System;
using System.Numerics;
using OpenCompositeConfigurator;

namespace OpenCompositeConfigurator.BodyTracking
{
    public sealed class World3DLegCaptureDeriverOptions
    {
        public float MinimumLandmarkConfidence { get; set; } = 0.35f;
        public float WorldScale { get; set; } = 1f;
        public int VelocityResetGapMs { get; set; } =
            BodyTrackerFrameMux.DefaultStaleAfterMilliseconds;
    }

    public readonly struct World3DLegCapturePair
    {
        public World3DLegCapturePair(
            World3DLegCaptureFields left,
            World3DLegCaptureFields right)
        {
            Left = left;
            Right = right;
        }

        public World3DLegCaptureFields Left { get; }
        public World3DLegCaptureFields Right { get; }
    }

    /// <summary>
    /// Builds symmetric per-leg diagnostics directly from raw MediaPipe world
    /// landmarks. Geometry and velocity vectors use OCU sender coordinates:
    /// +X anatomical right, +Y up, +Z body-forward. Semantic state is copied
    /// from the synchronized caller-provided classifier and is explicitly tagged
    /// with the stateSource supplied to Derive (for example, "world3d").
    /// </summary>
    public sealed class World3DLegCaptureDeriver
    {
        private const float Epsilon = 0.0001f;
        private const float StrikeMinimumFootLiftMeters = 0.18f;
        private const float StrikeFullFootLiftMeters = 0.35f;
        private const float StrikeMinimumStraightness = 0.86f;
        private const float StrikeFullStraightness = 0.96f;
        private const float StrikeMinimumHorizontalShinMeters = 0.12f;

        private struct JointHistory
        {
            public bool Valid;
            public Vector3 Position;
        }

        private struct LegHistory
        {
            public JointHistory Hip;
            public JointHistory Knee;
            public JointHistory Ankle;
            public JointHistory Foot;
        }

        private readonly struct LegGeometry
        {
            public LegGeometry(
                bool hipValid,
                Vector3 hip,
                bool kneeValid,
                Vector3 knee,
                bool ankleValid,
                Vector3 ankle,
                bool footValid,
                Vector3 foot,
                bool groundValid,
                float groundY)
            {
                HipValid = hipValid;
                Hip = hip;
                KneeValid = kneeValid;
                Knee = knee;
                AnkleValid = ankleValid;
                Ankle = ankle;
                FootValid = footValid;
                Foot = foot;
                GroundValid = groundValid;
                GroundY = groundY;
            }

            public bool HipValid { get; }
            public Vector3 Hip { get; }
            public bool KneeValid { get; }
            public Vector3 Knee { get; }
            public bool AnkleValid { get; }
            public Vector3 Ankle { get; }
            public bool FootValid { get; }
            public Vector3 Foot { get; }
            public bool GroundValid { get; }
            public float GroundY { get; }
        }

        private readonly float _minimumConfidence;
        private readonly float _worldScale;
        private readonly int _velocityResetGapMs;
        private bool _continuityValid;
        private uint _sourceEpoch;
        private long _sequence;
        private long _timestampMs;
        private LegHistory _leftHistory;
        private LegHistory _rightHistory;

        public World3DLegCaptureDeriver(
            World3DLegCaptureDeriverOptions? options = null)
        {
            options ??= new World3DLegCaptureDeriverOptions();
            if (!float.IsFinite(options.MinimumLandmarkConfidence)
                || options.MinimumLandmarkConfidence < 0f
                || options.MinimumLandmarkConfidence > 1f)
                throw new ArgumentOutOfRangeException(nameof(options.MinimumLandmarkConfidence));
            if (!float.IsFinite(options.WorldScale) || options.WorldScale <= 0f)
                throw new ArgumentOutOfRangeException(nameof(options.WorldScale));
            if (options.VelocityResetGapMs <= 0)
                throw new ArgumentOutOfRangeException(nameof(options.VelocityResetGapMs));

            _minimumConfidence = options.MinimumLandmarkConfidence;
            _worldScale = options.WorldScale;
            _velocityResetGapMs = options.VelocityResetGapMs;
        }

        public void Reset()
        {
            _continuityValid = false;
            _sourceEpoch = 0;
            _sequence = 0;
            _timestampMs = 0;
            _leftHistory = default;
            _rightHistory = default;
        }

        public World3DLegCapturePair Derive(
            WorldLandmarkFrame frame,
            CameraLowerBodyClassification classification,
            string? stateSource = null)
        {
            ArgumentNullException.ThrowIfNull(frame);
            stateSource = string.IsNullOrWhiteSpace(stateSource)
                ? "caller" : stateSource;

            bool continuous = _continuityValid
                && frame.SourceEpoch == _sourceEpoch
                && frame.Sequence > _sequence
                && frame.TimestampMs > _timestampMs
                && frame.TimestampMs - _timestampMs <= _velocityResetGapMs;
            float inverseDeltaSeconds = continuous
                ? 1000f / (frame.TimestampMs - _timestampMs)
                : 0f;
            if (!continuous)
            {
                _leftHistory = default;
                _rightHistory = default;
            }

            bool leftGroundValid = TryGroundY(
                frame,
                MediaPipePoseLandmarkIndex.LeftAnkle,
                MediaPipePoseLandmarkIndex.LeftHeel,
                MediaPipePoseLandmarkIndex.LeftFootIndex,
                out float leftGroundY);
            bool rightGroundValid = TryGroundY(
                frame,
                MediaPipePoseLandmarkIndex.RightAnkle,
                MediaPipePoseLandmarkIndex.RightHeel,
                MediaPipePoseLandmarkIndex.RightFootIndex,
                out float rightGroundY);
            bool floorValid = leftGroundValid || rightGroundValid;
            float floorY = leftGroundValid && rightGroundValid
                ? MathF.Min(leftGroundY, rightGroundY)
                : leftGroundValid ? leftGroundY : rightGroundY;

            LegGeometry leftGeometry = ReadLeg(
                frame,
                MediaPipePoseLandmarkIndex.LeftHip,
                MediaPipePoseLandmarkIndex.LeftKnee,
                MediaPipePoseLandmarkIndex.LeftAnkle,
                MediaPipePoseLandmarkIndex.LeftHeel,
                MediaPipePoseLandmarkIndex.LeftFootIndex,
                floorValid,
                floorY);
            LegGeometry rightGeometry = ReadLeg(
                frame,
                MediaPipePoseLandmarkIndex.RightHip,
                MediaPipePoseLandmarkIndex.RightKnee,
                MediaPipePoseLandmarkIndex.RightAnkle,
                MediaPipePoseLandmarkIndex.RightHeel,
                MediaPipePoseLandmarkIndex.RightFootIndex,
                floorValid,
                floorY);

            World3DLegCaptureFields left = BuildFields(
                leftGeometry,
                classification.Left,
                classification.TimestampMs,
                stateSource,
                _leftHistory,
                continuous,
                inverseDeltaSeconds);
            World3DLegCaptureFields right = BuildFields(
                rightGeometry,
                classification.Right,
                classification.TimestampMs,
                stateSource,
                _rightHistory,
                continuous,
                inverseDeltaSeconds);

            _leftHistory = CreateHistory(leftGeometry);
            _rightHistory = CreateHistory(rightGeometry);
            _continuityValid = true;
            _sourceEpoch = frame.SourceEpoch;
            _sequence = frame.Sequence;
            _timestampMs = frame.TimestampMs;
            return new World3DLegCapturePair(left, right);
        }

        private LegGeometry ReadLeg(
            WorldLandmarkFrame frame,
            MediaPipePoseLandmarkIndex hipIndex,
            MediaPipePoseLandmarkIndex kneeIndex,
            MediaPipePoseLandmarkIndex ankleIndex,
            MediaPipePoseLandmarkIndex heelIndex,
            MediaPipePoseLandmarkIndex toeIndex,
            bool floorValid,
            float floorY)
        {
            bool hipValid = TryPoint(frame, hipIndex, out Vector3 hip);
            bool kneeValid = TryPoint(frame, kneeIndex, out Vector3 knee);
            bool ankleValid = TryPoint(frame, ankleIndex, out Vector3 ankle);
            bool heelValid = TryPoint(frame, heelIndex, out Vector3 heel);
            bool toeValid = TryPoint(frame, toeIndex, out Vector3 toe);
            bool footValid = ResolveFoot(
                ankleValid, ankle,
                heelValid, heel,
                toeValid, toe,
                out Vector3 foot);
            return new LegGeometry(
                hipValid, hip,
                kneeValid, knee,
                ankleValid, ankle,
                footValid, foot,
                floorValid, floorY);
        }

        private static World3DLegCaptureFields BuildFields(
            LegGeometry geometry,
            CameraLegClassification classification,
            long classificationTimestampMs,
            string stateSource,
            LegHistory history,
            bool continuous,
            float inverseDeltaSeconds)
        {
            bool geometryValid = geometry.HipValid
                && geometry.KneeValid
                && geometry.AnkleValid
                && geometry.FootValid;
            float hipKneeLength = 0f;
            float kneeAnkleLength = 0f;
            float kneeAngleDegrees = 0f;
            float straightness = 0f;
            float horizontalShin = 0f;
            Vector3 strikeBearing = Vector3.Zero;
            bool strikeBearingValid = false;
            float strikeWeight = 0f;

            if (geometryValid)
            {
                Vector3 kneeToHip = geometry.Hip - geometry.Knee;
                Vector3 kneeToAnkle = geometry.Ankle - geometry.Knee;
                hipKneeLength = kneeToHip.Length();
                kneeAnkleLength = kneeToAnkle.Length();
                if (hipKneeLength > Epsilon && kneeAnkleLength > Epsilon)
                {
                    float bendCosine = Math.Clamp(
                        Vector3.Dot(kneeToHip, kneeToAnkle)
                            / (hipKneeLength * kneeAnkleLength),
                        -1f,
                        1f);
                    kneeAngleDegrees = MathF.Acos(bendCosine) * 180f / MathF.PI;
                    straightness = -bendCosine;
                }

                Vector3 horizontal = new Vector3(kneeToAnkle.X, 0f, kneeToAnkle.Z);
                horizontalShin = horizontal.Length();
                float ankleLift = geometry.GroundValid
                    ? geometry.Ankle.Y - geometry.GroundY : 0f;
                if (geometry.GroundValid
                    && ankleLift >= StrikeMinimumFootLiftMeters
                    && straightness >= StrikeMinimumStraightness
                    && horizontalShin >= StrikeMinimumHorizontalShinMeters)
                {
                    strikeBearing = horizontal / horizontalShin;
                    strikeWeight = MathF.Min(
                        SmoothRange(
                            ankleLift,
                            StrikeMinimumFootLiftMeters,
                            StrikeFullFootLiftMeters),
                        SmoothRange(
                            straightness,
                            StrikeMinimumStraightness,
                            StrikeFullStraightness));
                    strikeBearingValid = strikeWeight > 0f;
                }
            }

            (bool hipVelocityValid, Vector3 hipVelocity) = Velocity(
                geometry.HipValid, geometry.Hip,
                history.Hip, continuous, inverseDeltaSeconds);
            (bool kneeVelocityValid, Vector3 kneeVelocity) = Velocity(
                geometry.KneeValid, geometry.Knee,
                history.Knee, continuous, inverseDeltaSeconds);
            (bool ankleVelocityValid, Vector3 ankleVelocity) = Velocity(
                geometry.AnkleValid, geometry.Ankle,
                history.Ankle, continuous, inverseDeltaSeconds);
            (bool footVelocityValid, Vector3 footVelocity) = Velocity(
                geometry.FootValid, geometry.Foot,
                history.Foot, continuous, inverseDeltaSeconds);

            return new World3DLegCaptureFields(
                isValid: geometryValid,
                stateCode: (int)classification.TransportState,
                stateLabel: classification.TransportState.ToString(),
                stateSource: stateSource,
                stateTimestampMs: classificationTimestampMs,
                stateConfidence: classification.Confidence,
                baselineReady: classification.BaselineReady,
                blocksGait: classification.BlocksGait,
                stepOnset: classification.StepOnset,
                anatomicalFlipRejected: classification.AnatomicalFlipRejected,
                groundReferenceValid: geometry.GroundValid,
                groundReferenceYMeters: geometry.GroundValid ? geometry.GroundY : 0f,
                kneeAngleDegrees: kneeAngleDegrees,
                straightness: straightness,
                hipKneeLengthMeters: hipKneeLength,
                kneeAnkleLengthMeters: kneeAnkleLength,
                ankleLiftMeters: geometry.GroundValid && geometry.AnkleValid
                    ? geometry.Ankle.Y - geometry.GroundY : 0f,
                footLiftMeters: geometry.GroundValid && geometry.FootValid
                    ? geometry.Foot.Y - geometry.GroundY : 0f,
                horizontalShinMeters: horizontalShin,
                strikeBearingValid: strikeBearingValid,
                strikeBearing: strikeBearing,
                strikeWeight: strikeWeight,
                hipVelocityValid: hipVelocityValid,
                hipVelocityMetersPerSecond: hipVelocity,
                kneeVelocityValid: kneeVelocityValid,
                kneeVelocityMetersPerSecond: kneeVelocity,
                ankleVelocityValid: ankleVelocityValid,
                ankleVelocityMetersPerSecond: ankleVelocity,
                footVelocityValid: footVelocityValid,
                footVelocityMetersPerSecond: footVelocity);
        }

        private bool TryPoint(
            WorldLandmarkFrame frame,
            MediaPipePoseLandmarkIndex index,
            out Vector3 point)
        {
            WorldLandmark landmark = frame[(int)index];
            if (!landmark.IsWorldUsable(_minimumConfidence))
            {
                point = Vector3.Zero;
                return false;
            }

            Vector3 raw = landmark.WorldPosition * _worldScale;
            float xSign = frame.InputMirrored ? -1f : 1f;
            point = new Vector3(xSign * raw.X, -raw.Y, -raw.Z);
            if (!WorldLandmark.IsFinite(point))
            {
                point = Vector3.Zero;
                return false;
            }
            return true;
        }

        private bool TryGroundY(
            WorldLandmarkFrame frame,
            MediaPipePoseLandmarkIndex ankleIndex,
            MediaPipePoseLandmarkIndex heelIndex,
            MediaPipePoseLandmarkIndex toeIndex,
            out float groundY)
        {
            groundY = float.PositiveInfinity;
            if (TryPoint(frame, ankleIndex, out Vector3 ankle))
                groundY = MathF.Min(groundY, ankle.Y);
            if (TryPoint(frame, heelIndex, out Vector3 heel))
                groundY = MathF.Min(groundY, heel.Y);
            if (TryPoint(frame, toeIndex, out Vector3 toe))
                groundY = MathF.Min(groundY, toe.Y);
            return float.IsFinite(groundY);
        }

        private static bool ResolveFoot(
            bool ankleValid,
            Vector3 ankle,
            bool heelValid,
            Vector3 heel,
            bool toeValid,
            Vector3 toe,
            out Vector3 foot)
        {
            if (ankleValid)
            {
                foot = ankle;
                return true;
            }
            if (heelValid && toeValid)
            {
                foot = 0.5f * (heel + toe);
                return true;
            }
            foot = Vector3.Zero;
            return false;
        }

        private static LegHistory CreateHistory(LegGeometry geometry) => new()
        {
            Hip = new JointHistory { Valid = geometry.HipValid, Position = geometry.Hip },
            Knee = new JointHistory { Valid = geometry.KneeValid, Position = geometry.Knee },
            Ankle = new JointHistory { Valid = geometry.AnkleValid, Position = geometry.Ankle },
            Foot = new JointHistory { Valid = geometry.FootValid, Position = geometry.Foot },
        };

        private static (bool Valid, Vector3 Value) Velocity(
            bool currentValid,
            Vector3 current,
            JointHistory previous,
            bool continuous,
            float inverseDeltaSeconds)
        {
            bool valid = continuous
                && currentValid
                && previous.Valid
                && float.IsFinite(inverseDeltaSeconds)
                && inverseDeltaSeconds > 0f;
            if (!valid)
                return (false, Vector3.Zero);

            Vector3 velocity = (current - previous.Position) * inverseDeltaSeconds;
            return WorldLandmark.IsFinite(velocity)
                ? (true, velocity)
                : (false, Vector3.Zero);
        }

        private static float SmoothRange(float value, float minimum, float maximum)
        {
            if (value <= minimum)
                return 0f;
            if (value >= maximum)
                return 1f;
            float t = (value - minimum) / (maximum - minimum);
            return t * t * (3f - 2f * t);
        }
    }
}
