using System;
using System.Numerics;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>Stable MediaPipe Pose landmark numbers; never infer sides from x.</summary>
    public enum MediaPipe33LandmarkIndex : int
    {
        Nose = 0,
        LeftEyeInner = 1,
        LeftEye = 2,
        LeftEyeOuter = 3,
        RightEyeInner = 4,
        RightEye = 5,
        RightEyeOuter = 6,
        LeftEar = 7,
        RightEar = 8,
        MouthLeft = 9,
        MouthRight = 10,
        LeftShoulder = 11,
        RightShoulder = 12,
        LeftElbow = 13,
        RightElbow = 14,
        LeftWrist = 15,
        RightWrist = 16,
        LeftPinky = 17,
        RightPinky = 18,
        LeftIndex = 19,
        RightIndex = 20,
        LeftThumb = 21,
        RightThumb = 22,
        LeftHip = 23,
        RightHip = 24,
        LeftKnee = 25,
        RightKnee = 26,
        LeftAnkle = 27,
        RightAnkle = 28,
        LeftHeel = 29,
        RightHeel = 30,
        LeftFootIndex = 31,
        RightFootIndex = 32,
    }

    public sealed class MediaPipe33TrackerConverterOptions
    {
        public float MinimumLandmarkConfidence { get; set; } = 0.35f;
        public float GeometryEpsilonMeters { get; set; } = 0.0001f;
        public float ShoulderToHeadMeters { get; set; } = 0.24f;
        public float WorldScale { get; set; } = 1f;
        public bool IncludeAuxiliaryTrackers { get; set; } = true;
        public bool EnableAdaptiveStabilization { get; set; } = true;
        public int StabilizationResetGapMs { get; set; } =
            BodyTrackerFrameMux.DefaultStaleAfterMilliseconds;
    }

    /// <summary>
    /// Converts MediaPipe's hip-centred 33-landmark world skeleton into OCU's
    /// floor-relative Unity sender convention: +x anatomical right, +y up and
    /// +z body-forward. MediaPipe world X is already anatomical right, while
    /// its Y/Z axes are opposite OCU. Only an actually mirrored inference
    /// input reverses X; front-camera placement alone does not.
    ///
    /// Camera landmarks are stabilized here, before they become virtual Vive
    /// trackers. This deliberately does not touch physical tracker input. The
    /// filter works on head-relative offsets so every camera tracker shares one
    /// coherent root under the HMD; fast strides/kicks bypass the resting filter.
    /// </summary>
    public sealed class MediaPipe33TrackerConverter
    {
        private struct OrientationMemory
        {
            public bool Valid;
            public Quaternion Value;
        }

        private struct AdaptiveVectorMemory
        {
            public bool Valid;
            public Vector3 Value;
            public Vector3 RawValue;
            public Vector3 Derivative;
            public long TimestampMs;
        }

        private struct AdaptiveScalarMemory
        {
            public bool Valid;
            public float Value;
            public float RawValue;
            public float Derivative;
            public long TimestampMs;
        }

        private readonly struct AdaptiveFilterProfile
        {
            public AdaptiveFilterProfile(
                float restCutoffHz,
                float motionCutoffHz,
                float velocityBeta,
                float fastStepMeters,
                float fastAlpha)
            {
                RestCutoffHz = restCutoffHz;
                MotionCutoffHz = motionCutoffHz;
                VelocityBeta = velocityBeta;
                FastStepMeters = fastStepMeters;
                FastAlpha = fastAlpha;
            }

            public float RestCutoffHz { get; }
            public float MotionCutoffHz { get; }
            public float VelocityBeta { get; }
            public float FastStepMeters { get; }
            public float FastAlpha { get; }
        }

        // Root and floor are deliberately conservative: the real HMD owns the
        // playspace root. Relative feet/knees use a wider bandwidth. Filtering
        // is per axis: a fast forward kick can pass Z unchanged without also
        // releasing unrelated sideways X jitter.
        private const float DerivativeCutoffHz = 2f;
        private static readonly AdaptiveFilterProfile RootFilterProfile = new(
            restCutoffHz: 0.65f,
            motionCutoffHz: 10f,
            velocityBeta: 2f,
            fastStepMeters: 0.100f,
            fastAlpha: 0.85f);
        private static readonly AdaptiveFilterProfile WaistFilterProfile = new(
            restCutoffHz: 1.0f,
            motionCutoffHz: 16f,
            velocityBeta: 4f,
            fastStepMeters: 0.080f,
            fastAlpha: 0.95f);
        private static readonly AdaptiveFilterProfile FootFilterProfile = new(
            restCutoffHz: 0.9f,
            motionCutoffHz: 24f,
            velocityBeta: 6f,
            fastStepMeters: 0.060f,
            fastAlpha: 0.95f);
        private static readonly AdaptiveFilterProfile AuxiliaryFilterProfile = new(
            restCutoffHz: 1.2f,
            motionCutoffHz: 20f,
            velocityBeta: 5f,
            fastStepMeters: 0.060f,
            fastAlpha: 0.93f);
        private static readonly AdaptiveFilterProfile FloorFilterProfile = new(
            restCutoffHz: 0.45f,
            motionCutoffHz: 8f,
            velocityBeta: 1.5f,
            fastStepMeters: 0.100f,
            fastAlpha: 0.80f);

        private readonly float _minimumConfidence;
        private readonly float _epsilon;
        private readonly float _shoulderToHead;
        private readonly float _worldScale;
        private readonly bool _includeAuxiliary;
        private readonly bool _stabilizationEnabled;
        private readonly int _stabilizationResetGapMs;

        private bool _epochValid;
        private uint _epoch;
        private bool _continuityValid;
        private long _lastSequence;
        private long _lastTimestampMs;
        private bool _floorValid;
        private float _floorY;
        private AdaptiveScalarMemory _floorFilter;
        private AdaptiveVectorMemory _rootPositionFilter;
        private readonly AdaptiveVectorMemory[] _relativePositionFilters =
            new AdaptiveVectorMemory[BodyTrackerFrame.TrackerSlotCount];
        private OrientationMemory _waistOrientation;

        public MediaPipe33TrackerConverter(
            MediaPipe33TrackerConverterOptions? options = null)
        {
            options ??= new MediaPipe33TrackerConverterOptions();
            if (!float.IsFinite(options.MinimumLandmarkConfidence)
                || options.MinimumLandmarkConfidence < 0f
                || options.MinimumLandmarkConfidence > 1f)
                throw new ArgumentOutOfRangeException(nameof(options.MinimumLandmarkConfidence));
            if (!float.IsFinite(options.GeometryEpsilonMeters)
                || options.GeometryEpsilonMeters <= 0f)
                throw new ArgumentOutOfRangeException(nameof(options.GeometryEpsilonMeters));
            if (!float.IsFinite(options.ShoulderToHeadMeters)
                || options.ShoulderToHeadMeters <= 0f)
                throw new ArgumentOutOfRangeException(nameof(options.ShoulderToHeadMeters));
            if (!float.IsFinite(options.WorldScale) || options.WorldScale <= 0f)
                throw new ArgumentOutOfRangeException(nameof(options.WorldScale));
            if (options.StabilizationResetGapMs <= 0)
                throw new ArgumentOutOfRangeException(nameof(options.StabilizationResetGapMs));

            _minimumConfidence = options.MinimumLandmarkConfidence;
            _epsilon = options.GeometryEpsilonMeters;
            _shoulderToHead = options.ShoulderToHeadMeters;
            _worldScale = options.WorldScale;
            _includeAuxiliary = options.IncludeAuxiliaryTrackers;
            _stabilizationEnabled = options.EnableAdaptiveStabilization;
            _stabilizationResetGapMs = options.StabilizationResetGapMs;
        }

        public void Reset()
        {
            _epochValid = false;
            _epoch = 0;
            ResetTrackingContinuity();
        }

        private void ResetTrackingContinuity()
        {
            _continuityValid = false;
            _lastSequence = 0;
            _lastTimestampMs = 0;
            _floorValid = false;
            _floorY = 0f;
            _floorFilter = default;
            _rootPositionFilter = default;
            Array.Clear(_relativePositionFilters);
            _waistOrientation = default;
        }

        public BodyTrackerFrame Convert(WorldLandmarkFrame frame)
        {
            ArgumentNullException.ThrowIfNull(frame);
            if (!_epochValid || frame.SourceEpoch != _epoch)
            {
                Reset();
                _epochValid = true;
                _epoch = frame.SourceEpoch;
            }
            else if (_continuityValid
                && (frame.Sequence <= _lastSequence
                    || frame.TimestampMs <= _lastTimestampMs
                    || frame.TimestampMs - _lastTimestampMs > _stabilizationResetGapMs))
            {
                // Repeated/backwards frames and a camera/model stall are a new
                // acquisition, even when the worker has not advanced its epoch.
                ResetTrackingContinuity();
            }

            bool leftHipValid = TryPoint(frame, MediaPipe33LandmarkIndex.LeftHip,
                out Vector3 leftHip, out float leftHipConfidence);
            bool rightHipValid = TryPoint(frame, MediaPipe33LandmarkIndex.RightHip,
                out Vector3 rightHip, out float rightHipConfidence);
            bool leftShoulderValid = TryPoint(frame, MediaPipe33LandmarkIndex.LeftShoulder,
                out Vector3 leftShoulder, out float leftShoulderConfidence);
            bool rightShoulderValid = TryPoint(frame, MediaPipe33LandmarkIndex.RightShoulder,
                out Vector3 rightShoulder, out float rightShoulderConfidence);

            bool hipCenterValid = leftHipValid && rightHipValid;
            bool shoulderCenterValid = leftShoulderValid && rightShoulderValid;
            Vector3 hipCenter = hipCenterValid
                ? 0.5f * (leftHip + rightHip) : Vector3.Zero;
            Vector3 shoulderCenter = shoulderCenterValid
                ? 0.5f * (leftShoulder + rightShoulder) : Vector3.Zero;

            float hipConfidence = Minimum(leftHipConfidence, rightHipConfidence);
            float shoulderConfidence = Minimum(leftShoulderConfidence, rightShoulderConfidence);

            Vector3 bodyUp = Vector3.UnitY;
            bool torsoUpMeasured = hipCenterValid && shoulderCenterValid
                && TryNormalize(shoulderCenter - hipCenter, out bodyUp);

            Quaternion waistRaw = Quaternion.Identity;
            bool waistMeasured = hipCenterValid && torsoUpMeasured
                && TryWaistOrientation(leftHip, rightHip, shoulderCenter,
                    hipCenter, out waistRaw);
            bool waistRotationValid;
            Quaternion waistOrientation;
            if (waistMeasured)
            {
                waistOrientation = PreserveHemisphere(
                    waistRaw, ref _waistOrientation);
                waistRotationValid = true;
                bodyUp = Vector3.Transform(Vector3.UnitY, waistOrientation);
            }
            else if (_waistOrientation.Valid)
            {
                waistOrientation = _waistOrientation.Value;
                waistRotationValid = true;
                bodyUp = Vector3.Transform(Vector3.UnitY, waistOrientation);
            }
            else
            {
                waistOrientation = Quaternion.Identity;
                waistRotationValid = false;
            }
            bool leftAnkleValid = TryPoint(frame, MediaPipe33LandmarkIndex.LeftAnkle,
                out Vector3 leftAnkle, out float leftAnkleConfidence);
            bool rightAnkleValid = TryPoint(frame, MediaPipe33LandmarkIndex.RightAnkle,
                out Vector3 rightAnkle, out float rightAnkleConfidence);
            bool leftHeelValid = TryPoint(frame, MediaPipe33LandmarkIndex.LeftHeel,
                out Vector3 leftHeel, out float leftHeelConfidence);
            bool rightHeelValid = TryPoint(frame, MediaPipe33LandmarkIndex.RightHeel,
                out Vector3 rightHeel, out float rightHeelConfidence);
            bool leftToeValid = TryPoint(frame, MediaPipe33LandmarkIndex.LeftFootIndex,
                out Vector3 leftToe, out float leftToeConfidence);
            bool rightToeValid = TryPoint(frame, MediaPipe33LandmarkIndex.RightFootIndex,
                out Vector3 rightToe, out float rightToeConfidence);

            bool leftFootPositionValid = ResolveFootPosition(
                leftAnkleValid, leftAnkle, leftAnkleConfidence,
                leftHeelValid, leftHeel, leftHeelConfidence,
                leftToeValid, leftToe, leftToeConfidence,
                out Vector3 leftFootPosition, out float leftFootPositionConfidence);
            bool rightFootPositionValid = ResolveFootPosition(
                rightAnkleValid, rightAnkle, rightAnkleConfidence,
                rightHeelValid, rightHeel, rightHeelConfidence,
                rightToeValid, rightToe, rightToeConfidence,
                out Vector3 rightFootPosition, out float rightFootPositionConfidence);

            bool haveLeftGround = TryGroundY(
                leftAnkleValid, leftAnkle,
                leftHeelValid, leftHeel,
                leftToeValid, leftToe,
                out float leftGroundY);
            bool haveRightGround = TryGroundY(
                rightAnkleValid, rightAnkle,
                rightHeelValid, rightHeel,
                rightToeValid, rightToe,
                out float rightGroundY);
            if (haveLeftGround || haveRightGround)
            {
                float floorCandidate = haveLeftGround && haveRightGround
                    ? MathF.Min(leftGroundY, rightGroundY)
                    : haveLeftGround ? leftGroundY : rightGroundY;
                _floorY = StabilizeScalar(
                    floorCandidate,
                    frame.TimestampMs,
                    ref _floorFilter,
                    FloorFilterProfile);
                _floorValid = true;
            }

            bool leftKneeValid = TryPoint(frame, MediaPipe33LandmarkIndex.LeftKnee,
                out Vector3 leftKnee, out float leftKneeConfidence);
            bool rightKneeValid = TryPoint(frame, MediaPipe33LandmarkIndex.RightKnee,
                out Vector3 rightKnee, out float rightKneeConfidence);

            // These are virtual ankle targets, not arbitrarily mounted Vive
            // pucks. SkyrimVR-FBT stores a tracker-local calibration offset and
            // rotates that offset by every later tracker orientation. Feeding
            // it independent heel/toe or shin yaw therefore makes a straight
            // camera kick orbit sideways, and also steers its knee pole. Keep
            // both virtual feet flat and aligned to the body; the measured 3D
            // ankle positions continue to carry every stride and kick. Native
            // HTCX/Vive tracker poses never pass through this converter.
            Quaternion stableFootOrientation = Quaternion.Identity;
            bool stableFootRotationValid = waistRotationValid
                && TryVirtualFootOrientation(
                    waistOrientation, out stableFootOrientation);
            Quaternion leftFootOrientation = stableFootRotationValid
                ? stableFootOrientation : Quaternion.Identity;
            Quaternion rightFootOrientation = leftFootOrientation;
            bool leftFootMeasured = stableFootRotationValid && waistMeasured;
            bool rightFootMeasured = leftFootMeasured;
            bool leftFootRotationValid = leftFootPositionValid
                && stableFootRotationValid;
            bool rightFootRotationValid = rightFootPositionValid
                && stableFootRotationValid;

            bool corePoseValid = hipCenterValid
                && shoulderCenterValid
                && leftFootPositionValid
                && rightFootPositionValid
                && _floorValid
                && waistRotationValid
                && leftFootRotationValid
                && rightFootRotationValid;
            if (!corePoseValid)
            {
                // Do not drag a pre-occlusion camera pose into the reacquired
                // body. The mux needs an explicitly invalid frame now, and the
                // next complete frame must initialize every filter from raw.
                ResetTrackingContinuity();
                return CreateInvalidFrame(frame);
            }

            // This is an alignment anchor, not a camera replacement for the
            // real HMD. Shoulder lean must not pull the entire tracker root in
            // the opposite direction: keep anchor X/Z directly over the hips
            // and use the shoulders only to estimate its vertical height.
            Vector3 syntheticHead = new Vector3(
                hipCenter.X,
                shoulderCenter.Y + _shoulderToHead,
                hipCenter.Z);
            Vector3 rawHeadPosition = FloorRelative(syntheticHead);
            Vector3 stabilizedHeadPosition = StabilizeVector(
                rawHeadPosition,
                frame.TimestampMs,
                ref _rootPositionFilter,
                RootFilterProfile);

            BodyTrackerPose[] trackers = new BodyTrackerPose[BodyTrackerFrame.TrackerSlotCount];
            trackers[0] = MakePose(
                true,
                StabilizeRelativePosition(
                    hipCenter,
                    rawHeadPosition,
                    stabilizedHeadPosition,
                    frame.TimestampMs,
                    ref _relativePositionFilters[0],
                    WaistFilterProfile),
                waistRotationValid,
                waistOrientation,
                Minimum(hipConfidence, shoulderConfidence)
                    * (waistMeasured ? 1f : 0.55f));
            trackers[1] = MakePose(
                true,
                StabilizeRelativePosition(
                    leftFootPosition,
                    rawHeadPosition,
                    stabilizedHeadPosition,
                    frame.TimestampMs,
                    ref _relativePositionFilters[1],
                    FootFilterProfile),
                leftFootRotationValid,
                leftFootOrientation,
                Minimum(leftFootPositionConfidence, leftKneeConfidence)
                    * (leftFootMeasured ? 1f : 0.65f));
            trackers[2] = MakePose(
                true,
                StabilizeRelativePosition(
                    rightFootPosition,
                    rawHeadPosition,
                    stabilizedHeadPosition,
                    frame.TimestampMs,
                    ref _relativePositionFilters[2],
                    FootFilterProfile),
                rightFootRotationValid,
                rightFootOrientation,
                Minimum(rightFootPositionConfidence, rightKneeConfidence)
                    * (rightFootMeasured ? 1f : 0.65f));

            if (_includeAuxiliary)
            {
                trackers[3] = PositionOnly(leftKneeValid && _floorValid,
                    StabilizeRelativePosition(
                        leftKneeValid,
                        leftKnee,
                        rawHeadPosition,
                        stabilizedHeadPosition,
                        frame.TimestampMs,
                        ref _relativePositionFilters[3],
                        AuxiliaryFilterProfile),
                    leftKneeConfidence);
                trackers[4] = PositionOnly(rightKneeValid && _floorValid,
                    StabilizeRelativePosition(
                        rightKneeValid,
                        rightKnee,
                        rawHeadPosition,
                        stabilizedHeadPosition,
                        frame.TimestampMs,
                        ref _relativePositionFilters[4],
                        AuxiliaryFilterProfile),
                    rightKneeConfidence);

                bool leftElbowValid = TryPoint(frame, MediaPipe33LandmarkIndex.LeftElbow,
                    out Vector3 leftElbow, out float leftElbowConfidence);
                bool rightElbowValid = TryPoint(frame, MediaPipe33LandmarkIndex.RightElbow,
                    out Vector3 rightElbow, out float rightElbowConfidence);
                trackers[5] = PositionOnly(leftElbowValid && _floorValid,
                    StabilizeRelativePosition(
                        leftElbowValid,
                        leftElbow,
                        rawHeadPosition,
                        stabilizedHeadPosition,
                        frame.TimestampMs,
                        ref _relativePositionFilters[5],
                        AuxiliaryFilterProfile),
                    leftElbowConfidence);
                trackers[6] = PositionOnly(rightElbowValid && _floorValid,
                    StabilizeRelativePosition(
                        rightElbowValid,
                        rightElbow,
                        rawHeadPosition,
                        stabilizedHeadPosition,
                        frame.TimestampMs,
                        ref _relativePositionFilters[6],
                        AuxiliaryFilterProfile),
                    rightElbowConfidence);
                trackers[7] = PositionOnly(shoulderCenterValid && _floorValid,
                    StabilizeRelativePosition(
                        shoulderCenter,
                        rawHeadPosition,
                        stabilizedHeadPosition,
                        frame.TimestampMs,
                        ref _relativePositionFilters[7],
                        AuxiliaryFilterProfile),
                    shoulderConfidence);
            }

            BodyTrackerPose head = MakePose(
                true,
                stabilizedHeadPosition,
                waistRotationValid,
                waistOrientation,
                Minimum(hipConfidence, shoulderConfidence)
                    * (waistMeasured ? 1f : 0.55f));

            BodyPoseSourceFlags flags = frame.SourceFlags
                | BodyPoseSourceFlags.Continuous3D
                | BodyPoseSourceFlags.MediaPipe33
                | BodyPoseSourceFlags.MetricWorldCoordinates;
            _continuityValid = true;
            _lastSequence = frame.Sequence;
            _lastTimestampMs = frame.TimestampMs;
            return new BodyTrackerFrame(
                frame.Sequence,
                frame.TimestampMs,
                frame.SourceEpoch,
                BodyTrackerSource.Continuous3D,
                flags,
                head,
                trackers);
        }

        private bool TryPoint(
            WorldLandmarkFrame frame,
            MediaPipe33LandmarkIndex index,
            out Vector3 point,
            out float confidence)
        {
            WorldLandmark landmark = frame[(int)index];
            confidence = landmark.Confidence;
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

        private bool TryWaistOrientation(
            Vector3 leftHip,
            Vector3 rightHip,
            Vector3 shoulderCenter,
            Vector3 hipCenter,
            out Quaternion orientation)
        {
            if (!TryNormalize(shoulderCenter - hipCenter, out Vector3 up))
            {
                orientation = Quaternion.Identity;
                return false;
            }

            Vector3 rightCandidate = rightHip - leftHip;
            rightCandidate -= up * Vector3.Dot(rightCandidate, up);
            if (!TryNormalize(rightCandidate, out Vector3 right))
            {
                orientation = Quaternion.Identity;
                return false;
            }
            if (!TryNormalize(Vector3.Cross(right, up), out Vector3 forward))
            {
                orientation = Quaternion.Identity;
                return false;
            }
            if (!TryNormalize(Vector3.Cross(forward, right), out up))
            {
                orientation = Quaternion.Identity;
                return false;
            }
            return TryQuaternionFromAxes(right, up, forward, out orientation);
        }

        private bool TryVirtualFootOrientation(
            Quaternion waistOrientation,
            out Quaternion orientation)
        {
            // Keep the virtual tracker flat. Heel/toe and lower-leg rotation
            // are useful for rendering a foot, but SkyrimVR-FBT interprets the
            // tracker rotation as the frame for its saved mount offset. Using
            // those noisy rotations here physically moves the solved target.
            Vector3 forward = Vector3.Transform(
                Vector3.UnitZ, waistOrientation);
            forward.Y = 0f;
            if (!TryNormalize(forward, out forward)
                || !TryNormalize(
                    Vector3.Cross(Vector3.UnitY, forward),
                    out Vector3 right)
                || !TryQuaternionFromAxes(
                    right, Vector3.UnitY, forward, out orientation))
            {
                orientation = Quaternion.Identity;
                return false;
            }

            // Quaternion construction may choose the opposite hemisphere at
            // +/-180 degrees. Follow the already-continuous waist quaternion
            // so calibration offsets cannot see a representational flip.
            if (Quaternion.Dot(orientation, waistOrientation) < 0f)
                orientation = Negate(orientation);
            return true;
        }

        private bool TryQuaternionFromAxes(
            Vector3 right,
            Vector3 up,
            Vector3 forward,
            out Quaternion orientation)
        {
            // System.Numerics uses row-vector transforms. Rows therefore hold
            // the world directions of local +X, +Y and +Z respectively.
            float determinant = Vector3.Dot(right, Vector3.Cross(up, forward));
            if (!float.IsFinite(determinant) || determinant < 0.98f)
            {
                orientation = Quaternion.Identity;
                return false;
            }

            var matrix = new Matrix4x4(
                right.X, right.Y, right.Z, 0f,
                up.X, up.Y, up.Z, 0f,
                forward.X, forward.Y, forward.Z, 0f,
                0f, 0f, 0f, 1f);
            Quaternion candidate = Quaternion.CreateFromRotationMatrix(matrix);
            if (!BodyTrackerPose.IsFinite(candidate)
                || candidate.LengthSquared() <= _epsilon * _epsilon)
            {
                orientation = Quaternion.Identity;
                return false;
            }
            orientation = Quaternion.Normalize(candidate);
            return true;
        }

        private Quaternion PreserveHemisphere(
            Quaternion value,
            ref OrientationMemory memory)
        {
            value = Quaternion.Normalize(value);
            if (memory.Valid && Quaternion.Dot(value, memory.Value) < 0f)
            {
                value = new Quaternion(-value.X, -value.Y, -value.Z, -value.W);
            }
            memory.Valid = true;
            memory.Value = value;
            return value;
        }

        private static Quaternion Negate(Quaternion value) =>
            new(-value.X, -value.Y, -value.Z, -value.W);

        private bool TryNormalize(Vector3 value, out Vector3 normalized)
        {
            float lengthSquared = value.LengthSquared();
            if (!float.IsFinite(lengthSquared)
                || lengthSquared <= _epsilon * _epsilon)
            {
                normalized = Vector3.Zero;
                return false;
            }
            normalized = value / MathF.Sqrt(lengthSquared);
            return WorldLandmark.IsFinite(normalized);
        }

        private Vector3 FloorRelative(Vector3 point)
        {
            if (!_floorValid || !WorldLandmark.IsFinite(point))
                return Vector3.Zero;
            point.Y -= _floorY;
            return point;
        }

        private Vector3 StabilizeRelativePosition(
            Vector3 point,
            Vector3 rawHeadPosition,
            Vector3 stabilizedHeadPosition,
            long timestampMs,
            ref AdaptiveVectorMemory memory,
            AdaptiveFilterProfile profile) =>
            StabilizeRelativePosition(
                true,
                point,
                rawHeadPosition,
                stabilizedHeadPosition,
                timestampMs,
                ref memory,
                profile);

        private Vector3 StabilizeRelativePosition(
            bool valid,
            Vector3 point,
            Vector3 rawHeadPosition,
            Vector3 stabilizedHeadPosition,
            long timestampMs,
            ref AdaptiveVectorMemory memory,
            AdaptiveFilterProfile profile)
        {
            if (!valid || !_floorValid || !WorldLandmark.IsFinite(point))
            {
                memory = default;
                return Vector3.Zero;
            }

            // Filter body geometry, not independent camera-space roots. Adding
            // the same stabilized head root back to every tracker guarantees
            // that the runtime's head-to-HMD alignment cannot leave one leg or
            // the waist behind when the model origin jitters.
            Vector3 relative = FloorRelative(point) - rawHeadPosition;
            Vector3 stabilizedRelative = StabilizeVector(
                relative, timestampMs, ref memory, profile);
            return stabilizedHeadPosition + stabilizedRelative;
        }

        private Vector3 StabilizeVector(
            Vector3 sample,
            long timestampMs,
            ref AdaptiveVectorMemory memory,
            AdaptiveFilterProfile profile)
        {
            if (!_stabilizationEnabled
                || !memory.Valid
                || timestampMs <= memory.TimestampMs
                || timestampMs - memory.TimestampMs > _stabilizationResetGapMs)
            {
                memory.Valid = true;
                memory.Value = sample;
                memory.RawValue = sample;
                memory.Derivative = Vector3.Zero;
                memory.TimestampMs = timestampMs;
                return sample;
            }

            if (!WorldLandmark.IsFinite(sample))
            {
                memory = default;
                return Vector3.Zero;
            }

            float dt = Math.Clamp(
                (timestampMs - memory.TimestampMs) / 1000f,
                1f / 240f,
                0.1f);
            float x = memory.Value.X;
            float y = memory.Value.Y;
            float z = memory.Value.Z;
            float dx = memory.Derivative.X;
            float dy = memory.Derivative.Y;
            float dz = memory.Derivative.Z;
            FilterAxis(sample.X, memory.RawValue.X, ref x, ref dx, dt, profile);
            FilterAxis(sample.Y, memory.RawValue.Y, ref y, ref dy, dt, profile);
            FilterAxis(sample.Z, memory.RawValue.Z, ref z, ref dz, dt, profile);
            memory.Value = new Vector3(x, y, z);
            memory.RawValue = sample;
            memory.Derivative = new Vector3(dx, dy, dz);
            memory.TimestampMs = timestampMs;
            return memory.Value;
        }

        private float StabilizeScalar(
            float sample,
            long timestampMs,
            ref AdaptiveScalarMemory memory,
            AdaptiveFilterProfile profile)
        {
            if (!_stabilizationEnabled
                || !memory.Valid
                || timestampMs <= memory.TimestampMs
                || timestampMs - memory.TimestampMs > _stabilizationResetGapMs)
            {
                memory.Valid = true;
                memory.Value = sample;
                memory.RawValue = sample;
                memory.Derivative = 0f;
                memory.TimestampMs = timestampMs;
                return sample;
            }

            if (!float.IsFinite(sample))
            {
                memory = default;
                return sample;
            }

            float dt = Math.Clamp(
                (timestampMs - memory.TimestampMs) / 1000f,
                1f / 240f,
                0.1f);
            float value = memory.Value;
            float derivative = memory.Derivative;
            FilterAxis(
                sample,
                memory.RawValue,
                ref value,
                ref derivative,
                dt,
                profile);
            memory.Value = value;
            memory.RawValue = sample;
            memory.Derivative = derivative;
            memory.TimestampMs = timestampMs;
            return memory.Value;
        }

        private static void FilterAxis(
            float sample,
            float previousRaw,
            ref float filtered,
            ref float derivative,
            float dt,
            AdaptiveFilterProfile profile)
        {
            float rawStep = sample - previousRaw;
            float rawVelocity = rawStep / dt;
            float derivativeAlpha = LowPassAlpha(DerivativeCutoffHz, dt);
            derivative += derivativeAlpha * (rawVelocity - derivative);

            float cutoff = Math.Clamp(
                profile.RestCutoffHz + profile.VelocityBeta * MathF.Abs(derivative),
                profile.RestCutoffHz,
                profile.MotionCutoffHz);
            float alpha = LowPassAlpha(cutoff, dt);
            if (MathF.Abs(rawStep) >= profile.FastStepMeters)
                alpha = MathF.Max(alpha, profile.FastAlpha);
            filtered += alpha * (sample - filtered);
        }

        private static float LowPassAlpha(float cutoffHz, float dt) =>
            1f - MathF.Exp(-2f * MathF.PI * cutoffHz * dt);

        private static BodyTrackerFrame CreateInvalidFrame(WorldLandmarkFrame frame)
        {
            BodyPoseSourceFlags flags = frame.SourceFlags
                | BodyPoseSourceFlags.Continuous3D
                | BodyPoseSourceFlags.MediaPipe33
                | BodyPoseSourceFlags.MetricWorldCoordinates;
            return new BodyTrackerFrame(
                frame.Sequence,
                frame.TimestampMs,
                frame.SourceEpoch,
                BodyTrackerSource.Continuous3D,
                flags,
                BodyTrackerPose.Invalid,
                new BodyTrackerPose[BodyTrackerFrame.TrackerSlotCount]);
        }

        private static bool ResolveFootPosition(
            bool ankleValid,
            Vector3 ankle,
            float ankleConfidence,
            bool heelValid,
            Vector3 heel,
            float heelConfidence,
            bool toeValid,
            Vector3 toe,
            float toeConfidence,
            out Vector3 position,
            out float confidence)
        {
            if (ankleValid)
            {
                position = ankle;
                confidence = ankleConfidence;
                return true;
            }
            if (heelValid && toeValid)
            {
                position = 0.5f * (heel + toe);
                confidence = Minimum(heelConfidence, toeConfidence) * 0.75f;
                return true;
            }
            position = Vector3.Zero;
            confidence = 0f;
            return false;
        }

        private static bool TryGroundY(
            bool ankleValid,
            Vector3 ankle,
            bool heelValid,
            Vector3 heel,
            bool toeValid,
            Vector3 toe,
            out float y)
        {
            y = float.PositiveInfinity;
            if (ankleValid)
                y = MathF.Min(y, ankle.Y);
            if (heelValid)
                y = MathF.Min(y, heel.Y);
            if (toeValid)
                y = MathF.Min(y, toe.Y);
            return float.IsFinite(y);
        }

        private static BodyTrackerPose MakePose(
            bool positionValid,
            Vector3 position,
            bool rotationValid,
            Quaternion orientation,
            float confidence) =>
            new BodyTrackerPose(
                position,
                orientation,
                confidence,
                positionValid,
                rotationValid);

        private static BodyTrackerPose PositionOnly(
            bool valid,
            Vector3 position,
            float confidence) =>
            new BodyTrackerPose(
                position,
                Quaternion.Identity,
                confidence,
                valid,
                rotationValid: false);

        private static float Minimum(float a, float b) => MathF.Min(a, b);
    }
}
