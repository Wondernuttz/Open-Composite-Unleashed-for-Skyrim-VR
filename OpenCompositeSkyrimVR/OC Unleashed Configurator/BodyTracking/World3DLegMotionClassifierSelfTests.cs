using System;
using System.Collections.Generic;
using System.Numerics;

namespace OpenCompositeConfigurator.BodyTracking
{
    public sealed class World3DLegMotionClassifierSelfTestResult
    {
        internal World3DLegMotionClassifierSelfTestResult(List<string> failures)
        {
            Failures = failures.AsReadOnly();
        }

        public IReadOnlyList<string> Failures { get; }
        public bool Passed => Failures.Count == 0;
    }

    /// <summary>
    /// Deterministic geometry/temporal tests. These use only metric world
    /// landmarks; every face/head landmark remains invalid on purpose.
    /// </summary>
    public static class World3DLegMotionClassifierSelfTests
    {
        private readonly struct LegPose
        {
            public LegPose(Vector3 knee, Vector3 ankle)
            {
                Knee = knee;
                Ankle = ankle;
            }

            public Vector3 Knee { get; }
            public Vector3 Ankle { get; }
        }

        private sealed class SequenceBuilder
        {
            private long _sequence;
            private long _timestampMs;
            private readonly uint _epoch;

            public SequenceBuilder(uint epoch = 9)
            {
                _epoch = epoch;
            }

            public WorldLandmarkFrame Next(
                LegPose left,
                LegPose right,
                int advanceMs = 40,
                float leftConfidence = 1f,
                float rightConfidence = 1f)
            {
                _sequence++;
                _timestampMs += advanceMs;
                return CreateFrame(
                    _sequence,
                    _timestampMs,
                    _epoch,
                    left,
                    right,
                    leftConfidence,
                    rightConfidence);
            }
        }

        public static World3DLegMotionClassifierSelfTestResult Run()
        {
            var failures = new List<string>();
            TestHeadlessNeutralAndAlternatingGait(failures);
            TestSlowHighStrideStillAlternates(failures);
            TestFastRunCadenceStillAlternates(failures);
            TestRepeatedUnilateralStepsNeverBecomeGait(failures);
            TestRepeatedKicksNeverBecomeGait(failures);
            TestAlternatingKicksNeverBecomeGait(failures);
            TestObliqueKickIsDirectionInvariant(failures);
            TestLeftRightSymmetry(failures);
            TestKneeHoldDoesNotBecomeGait(failures);
            TestSynchronizedTrackerContract(failures);
            TestConvertedFloorLiftSurvivesHipRelativeCancellation(failures);
            TestForwardAxisUsesOcuConvention(failures);
            return new World3DLegMotionClassifierSelfTestResult(failures);
        }

        public static void AssertAll()
        {
            World3DLegMotionClassifierSelfTestResult result = Run();
            if (!result.Passed)
                throw new InvalidOperationException(
                    "World3D leg classifier self-test failed:\n - "
                    + string.Join("\n - ", result.Failures));
        }

        private static void TestHeadlessNeutralAndAlternatingGait(
            List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            CameraLowerBodyClassification result = WarmNeutral(classifier, frames);
            if (result.Left.State != CameraLegMotionState.Grounded
                || result.Right.State != CameraLegMotionState.Grounded)
            {
                failures.Add("Headless neutral stance did not establish two grounded legs.");
            }

            result = OrdinaryStep(classifier, frames, left: true);
            if (result.CoordinatedGait)
                failures.Add("One completed 3D step armed gait.");
            result = OrdinaryStep(classifier, frames, left: false);
            if (result.CoordinatedGait)
                failures.Add("Only one L/R alternation armed gait.");
            result = OrdinaryStep(classifier, frames, left: true);
            if (!result.CoordinatedGait)
                failures.Add("Three completed alternating 3D steps did not arm gait.");

            CameraLowerBodyClassification airborne = UpdateRepeated(
                classifier, frames, Neutral(left: true), WalkPose(left: false), 2);
            if (!airborne.CoordinatedGait
                || airborne.Right.State != CameraLegMotionState.WalkStep)
            {
                failures.Add($"A post-lock alternating lift was not emitted as WalkStep "
                    + $"(gait={airborne.CoordinatedGait}, state={airborne.Right.State}, "
                    + $"lift={airborne.Right.NormalizedAnkleLift:0.000}, "
                    + $"knee={airborne.Right.NormalizedKneeLift:0.000}).");
            }
        }

        private static void TestRepeatedUnilateralStepsNeverBecomeGait(
            List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WarmNeutral(classifier, frames);
            bool gait = false;
            for (int i = 0; i < 5; i++)
                gait |= OrdinaryStep(classifier, frames, left: true).CoordinatedGait;
            if (gait)
                failures.Add("Repeated same-side ordinary lifts accumulated into gait.");
        }

        private static void TestSlowHighStrideStillAlternates(
            List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WarmNeutral(classifier, frames);
            CameraLowerBodyClassification result = default;
            result = TimedStep(classifier, frames, true, HighMarchPose(true), 100, 6, 3);
            result = TimedStep(classifier, frames, false, HighMarchPose(false), 100, 6, 3);
            result = TimedStep(classifier, frames, true, HighMarchPose(true), 100, 6, 3);
            if (!result.CoordinatedGait)
                failures.Add("Slow alternating high strides were permanently treated as chambers.");
        }

        private static void TestFastRunCadenceStillAlternates(
            List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WarmNeutral(classifier, frames);
            CameraLowerBodyClassification result = default;
            result = TimedStep(classifier, frames, true, WalkPose(true), 25, 5, 3);
            result = TimedStep(classifier, frames, false, WalkPose(false), 25, 5, 3);
            result = TimedStep(classifier, frames, true, WalkPose(true), 25, 5, 3);
            if (!result.CoordinatedGait)
                failures.Add("Fast alternating 3D run cadence did not arm gait.");
        }

        private static void TestRepeatedKicksNeverBecomeGait(
            List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WarmNeutral(classifier, frames);
            bool gait = false;
            bool kickSeen = false;
            for (int i = 0; i < 3; i++)
            {
                RunKick(classifier, frames, left: true, yawDegrees: 0f,
                    ref gait, ref kickSeen);
                HoldNeutral(classifier, frames, 18, ref gait);
            }
            if (!kickSeen)
                failures.Add("Repeated forward kicks never emitted KickExtend.");
            if (gait)
                failures.Add("Repeated same-side kicks armed coordinated gait.");
        }

        private static void TestAlternatingKicksNeverBecomeGait(
            List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WarmNeutral(classifier, frames);
            bool gait = false;
            bool kickSeen = false;
            RunKick(classifier, frames, true, 0f, ref gait, ref kickSeen);
            HoldNeutral(classifier, frames, 18, ref gait);
            RunKick(classifier, frames, false, 0f, ref gait, ref kickSeen);
            HoldNeutral(classifier, frames, 18, ref gait);
            RunKick(classifier, frames, true, 0f, ref gait, ref kickSeen);
            if (!kickSeen)
                failures.Add("Alternating kick test did not exercise KickExtend.");
            if (gait)
                failures.Add("Alternating kicks were mistaken for alternating gait.");
        }

        private static void TestObliqueKickIsDirectionInvariant(
            List<string> failures)
        {
            CameraLegMotionState forward = PeakKickState(left: true, yawDegrees: 0f);
            CameraLegMotionState oblique = PeakKickState(left: true, yawDegrees: 45f);
            if (forward != CameraLegMotionState.KickExtend
                || oblique != CameraLegMotionState.KickExtend)
            {
                failures.Add($"Direction-invariant kick classification failed: "
                    + $"forward={forward}, left45={oblique}.");
            }
        }

        private static void TestLeftRightSymmetry(List<string> failures)
        {
            CameraLegMotionState left = PeakKickState(left: true, yawDegrees: 35f);
            CameraLegMotionState right = PeakKickState(left: false, yawDegrees: -35f);
            if (left != right || left != CameraLegMotionState.KickExtend)
            {
                failures.Add($"Mirrored left/right kick paths diverged: L={left}, R={right}.");
            }
        }

        private static void TestKneeHoldDoesNotBecomeGait(List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WarmNeutral(classifier, frames);
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < 9; i++)
                result = classifier.Update(frames.Next(
                    ChamberPose(left: true, 0f), Neutral(left: false)));
            if (result.Left.State != CameraLegMotionState.KneeHold)
                failures.Add($"Held chamber became {result.Left.State}, expected KneeHold.");
            if (result.CoordinatedGait || !result.SuppressGait)
                failures.Add("A held 3D knee chamber did not suppress gait.");
        }

        private static void TestSynchronizedTrackerContract(List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WorldLandmarkFrame frame = frames.Next(Neutral(true), Neutral(false));
            BodyTrackerFrame mismatched = CreateTrackers(
                frame.Sequence + 1, frame.TimestampMs, frame.SourceEpoch);
            CameraLowerBodyClassification result = classifier.Update(frame, mismatched);
            if (result.Left.TransportState != CameraLegMotionState.Invalid
                || result.Right.TransportState != CameraLegMotionState.Invalid)
            {
                failures.Add("A mismatched WorldLandmarkFrame/BodyTrackerFrame pair was accepted.");
            }

            classifier.Reset();
            frames = new SequenceBuilder();
            for (int i = 0; i < 10; i++)
            {
                frame = frames.Next(Neutral(true), Neutral(false));
                result = classifier.Update(frame, CreateTrackers(
                    frame.Sequence, frame.TimestampMs, frame.SourceEpoch,
                    headValid: false));
            }
            if (result.Left.State != CameraLegMotionState.Grounded
                || result.Right.State != CameraLegMotionState.Grounded)
            {
                failures.Add("A synchronized World3D tracker pair did not classify normally.");
            }
        }

        private static void TestConvertedFloorLiftSurvivesHipRelativeCancellation(
            List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder(epoch: 41);
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < 10; i++)
            {
                WorldLandmarkFrame neutral = frames.Next(Neutral(true), Neutral(false));
                result = classifier.Update(neutral, CreateTrackers(
                    neutral.Sequence, neutral.TimestampMs, neutral.SourceEpoch,
                    leftFootY: 0.08f, rightFootY: 0.11f,
                    leftKneeY: 0.51f, rightKneeY: 0.54f));
            }

            // Reproduce the live failure: the raw hip-relative right leg is
            // unchanged, while the synchronized floor-relative right foot has
            // plainly risen 19.5 cm. Production must trust the coherent
            // converted skeleton instead of reporting Grounded.
            WorldLandmarkFrame lifted = frames.Next(Neutral(true), Neutral(false));
            result = classifier.Update(lifted, CreateTrackers(
                lifted.Sequence, lifted.TimestampMs, lifted.SourceEpoch,
                leftFootY: 0.08f, rightFootY: 0.305f,
                leftKneeY: 0.51f, rightKneeY: 0.575f));
            if (result.Right.State != CameraLegMotionState.UndecidedLift
                || result.Right.NormalizedAnkleLift < 0.18f)
            {
                failures.Add($"Converted right-foot lift was lost to raw hip-relative cancellation: "
                    + $"state={result.Right.State}, lift={result.Right.NormalizedAnkleLift:0.000}.");
            }
            if (result.Left.State != CameraLegMotionState.Grounded)
                failures.Add("A right converted-floor lift disturbed the planted left leg.");
        }

        private static void TestForwardAxisUsesOcuConvention(List<string> failures)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder(epoch: 42);
            WarmNeutral(classifier, frames);
            CameraLowerBodyClassification result = UpdateRepeated(
                classifier, frames,
                ExtendedKickPose(left: true, yawDegrees: 0f),
                Neutral(left: false),
                2);
            if (result.Left.NormalizedDepthDelta <= 0f)
            {
                failures.Add($"A forward OCU kick reported reversed depth "
                    + $"{result.Left.NormalizedDepthDelta:0.000}.");
            }
        }

        private static World3DLegMotionClassifier NewClassifier() =>
            new World3DLegMotionClassifier(
                new World3DLegMotionClassifierOptions
                {
                    NeutralWarmupFrames = 6,
                    GaitLockMilliseconds = 1700,
                });

        private static CameraLowerBodyClassification WarmNeutral(
            World3DLegMotionClassifier classifier,
            SequenceBuilder frames)
        {
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < 10; i++)
                result = classifier.Update(frames.Next(Neutral(true), Neutral(false)));
            return result;
        }

        private static CameraLowerBodyClassification OrdinaryStep(
            World3DLegMotionClassifier classifier,
            SequenceBuilder frames,
            bool left)
        {
            LegPose moving = WalkPose(left);
            LegPose planted = Neutral(!left);
            CameraLowerBodyClassification result = UpdateRepeated(
                classifier, frames,
                left ? moving : planted,
                left ? planted : moving,
                6);
            result = UpdateRepeated(
                classifier, frames, Neutral(true), Neutral(false), 4);
            return result;
        }

        private static CameraLowerBodyClassification TimedStep(
            World3DLegMotionClassifier classifier,
            SequenceBuilder frames,
            bool left,
            LegPose moving,
            int frameMilliseconds,
            int movingFrames,
            int groundedFrames)
        {
            LegPose planted = Neutral(!left);
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < movingFrames; i++)
                result = classifier.Update(frames.Next(
                    left ? moving : planted,
                    left ? planted : moving,
                    frameMilliseconds));
            for (int i = 0; i < groundedFrames; i++)
                result = classifier.Update(frames.Next(
                    Neutral(true), Neutral(false), frameMilliseconds));
            return result;
        }

        private static CameraLowerBodyClassification UpdateRepeated(
            World3DLegMotionClassifier classifier,
            SequenceBuilder frames,
            LegPose left,
            LegPose right,
            int count)
        {
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < count; i++)
                result = classifier.Update(frames.Next(left, right));
            return result;
        }

        private static CameraLegMotionState PeakKickState(bool left, float yawDegrees)
        {
            var classifier = NewClassifier();
            var frames = new SequenceBuilder();
            WarmNeutral(classifier, frames);
            LegPose planted = Neutral(!left);
            LegPose chamber = ChamberPose(left, yawDegrees);
            LegPose extension = ExtendedKickPose(left, yawDegrees);
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < 4; i++)
                result = classifier.Update(frames.Next(
                    left ? chamber : planted,
                    left ? planted : chamber));
            for (int i = 0; i < 2; i++)
                result = classifier.Update(frames.Next(
                    left ? extension : planted,
                    left ? planted : extension));
            return left ? result.Left.State : result.Right.State;
        }

        private static CameraLowerBodyClassification RunKick(
            World3DLegMotionClassifier classifier,
            SequenceBuilder frames,
            bool left,
            float yawDegrees,
            ref bool gait,
            ref bool kickSeen)
        {
            LegPose planted = Neutral(!left);
            LegPose chamber = ChamberPose(left, yawDegrees);
            LegPose extension = ExtendedKickPose(left, yawDegrees);
            CameraLowerBodyClassification result = default;
            for (int i = 0; i < 4; i++)
            {
                result = classifier.Update(frames.Next(
                    left ? chamber : planted,
                    left ? planted : chamber));
                Observe(result, ref gait, ref kickSeen);
            }
            for (int i = 0; i < 4; i++)
            {
                result = classifier.Update(frames.Next(
                    left ? extension : planted,
                    left ? planted : extension));
                Observe(result, ref gait, ref kickSeen);
            }
            for (int i = 0; i < 5; i++)
            {
                result = classifier.Update(frames.Next(Neutral(true), Neutral(false)));
                Observe(result, ref gait, ref kickSeen);
            }
            return result;
        }

        private static void HoldNeutral(
            World3DLegMotionClassifier classifier,
            SequenceBuilder frames,
            int count,
            ref bool gait)
        {
            bool ignoredKick = false;
            for (int i = 0; i < count; i++)
            {
                CameraLowerBodyClassification result = classifier.Update(
                    frames.Next(Neutral(true), Neutral(false)));
                Observe(result, ref gait, ref ignoredKick);
            }
        }

        private static void Observe(
            in CameraLowerBodyClassification result,
            ref bool gait,
            ref bool kickSeen)
        {
            gait |= result.CoordinatedGait;
            kickSeen |= result.Left.State == CameraLegMotionState.KickExtend
                || result.Right.State == CameraLegMotionState.KickExtend;
        }

        private static LegPose Neutral(bool left)
        {
            float x = left ? -0.15f : 0.15f;
            return new LegPose(
                new Vector3(x, -0.43f, 0.015f),
                new Vector3(x, -0.86f, 0.030f));
        }

        private static LegPose WalkPose(bool left)
        {
            float x = left ? -0.15f : 0.15f;
            // A bent, low-knee stride: the ankle rises, but the knee does not
            // form the high chamber that precedes a kick.
            return new LegPose(
                new Vector3(x, -0.40f, 0.16f),
                new Vector3(x, -0.78f, -0.04f));
        }

        private static LegPose ChamberPose(bool left, float yawDegrees)
        {
            float x = left ? -0.15f : 0.15f;
            Vector2 direction = Direction(yawDegrees);
            Vector3 knee = new Vector3(
                x + direction.X * 0.403f,
                -0.150f,
                direction.Y * 0.403f);
            Vector3 ankle = knee + new Vector3(
                -direction.X * 0.250f,
                -0.350f,
                -direction.Y * 0.250f);
            return new LegPose(knee, ankle);
        }

        private static LegPose HighMarchPose(bool left) =>
            ChamberPose(left, 0f);

        private static LegPose ExtendedKickPose(bool left, float yawDegrees)
        {
            float x = left ? -0.15f : 0.15f;
            Vector2 direction = Direction(yawDegrees);
            Vector3 knee = new Vector3(
                x + direction.X * 0.403f,
                -0.150f,
                direction.Y * 0.403f);
            Vector3 ankle = knee + new Vector3(
                direction.X * 0.429f,
                -0.030f,
                direction.Y * 0.429f);
            return new LegPose(knee, ankle);
        }

        private static Vector2 Direction(float yawDegrees)
        {
            float radians = yawDegrees * MathF.PI / 180f;
            return new Vector2(MathF.Sin(radians), MathF.Cos(radians));
        }

        private static WorldLandmarkFrame CreateFrame(
            long sequence,
            long timestampMs,
            uint epoch,
            LegPose left,
            LegPose right,
            float leftConfidence,
            float rightConfidence)
        {
            var landmarks = new WorldLandmark[WorldLandmarkFrame.MediaPipeLandmarkCount];
            void Set(MediaPipe33LandmarkIndex index, Vector3 point, float confidence = 1f)
            {
                // Inverse of the production converter for unmirrored input.
                // MediaPipe world X is already anatomical right; Y/Z reverse.
                Vector3 raw = new Vector3(point.X, -point.Y, -point.Z);
                landmarks[(int)index] = new WorldLandmark(
                    Vector3.Zero, raw, confidence, true);
            }

            Set(MediaPipe33LandmarkIndex.LeftShoulder, new Vector3(-0.21f, 0.55f, 0f));
            Set(MediaPipe33LandmarkIndex.RightShoulder, new Vector3(0.21f, 0.55f, 0f));
            Set(MediaPipe33LandmarkIndex.LeftHip, new Vector3(-0.15f, 0f, 0f), leftConfidence);
            Set(MediaPipe33LandmarkIndex.RightHip, new Vector3(0.15f, 0f, 0f), rightConfidence);
            Set(MediaPipe33LandmarkIndex.LeftKnee, left.Knee, leftConfidence);
            Set(MediaPipe33LandmarkIndex.RightKnee, right.Knee, rightConfidence);
            Set(MediaPipe33LandmarkIndex.LeftAnkle, left.Ankle, leftConfidence);
            Set(MediaPipe33LandmarkIndex.RightAnkle, right.Ankle, rightConfidence);

            return new WorldLandmarkFrame(
                sequence,
                timestampMs,
                epoch,
                BodyPoseSourceFlags.NativeWorker,
                1280,
                720,
                false,
                landmarks);
        }

        private static BodyTrackerFrame CreateTrackers(
            long sequence,
            long timestampMs,
            uint epoch,
            bool headValid = true,
            float leftFootY = 0.08f,
            float rightFootY = 0.11f,
            float leftKneeY = 0.51f,
            float rightKneeY = 0.54f)
        {
            BodyTrackerPose Pose(Vector3 position) => new BodyTrackerPose(
                position, Quaternion.Identity, 1f, true, true);
            var trackers = new BodyTrackerPose[BodyTrackerFrame.TrackerSlotCount];
            trackers[0] = Pose(new Vector3(0f, 0.90f, 0f));
            trackers[1] = Pose(new Vector3(-0.15f, leftFootY, 0f));
            trackers[2] = Pose(new Vector3(0.15f, rightFootY, 0f));
            trackers[3] = Pose(new Vector3(-0.15f, leftKneeY, 0f));
            trackers[4] = Pose(new Vector3(0.15f, rightKneeY, 0f));
            for (int i = 5; i < trackers.Length; i++)
                trackers[i] = Pose(new Vector3(0f, 0.9f - i * 0.1f, 0f));
            return new BodyTrackerFrame(
                sequence,
                timestampMs,
                epoch,
                BodyTrackerSource.Continuous3D,
                BodyPoseSourceFlags.Continuous3D,
                headValid ? Pose(new Vector3(0f, 1.7f, 0f))
                    : BodyTrackerPose.Invalid,
                trackers);
        }
    }
}
