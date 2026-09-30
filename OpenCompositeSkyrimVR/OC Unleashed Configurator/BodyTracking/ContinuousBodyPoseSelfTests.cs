using System;
using System.Collections.Generic;
using System.Numerics;

namespace OpenCompositeConfigurator.BodyTracking
{
    /// <summary>
    /// Dependency-free deterministic checks for the 3D frame contract, mapping,
    /// rotation fallbacks and source mux. Call AssertAll() from a debug command,
    /// or RunAll() to display failures without terminating the app.
    /// </summary>
    public static class ContinuousBodyPoseSelfTests
    {
        public static IReadOnlyList<string> RunAll()
        {
            var failures = new List<string>();
            Run("immutable frame buffers", TestImmutableFrames, failures);
            Run("MediaPipe coordinate mapping", TestCoordinateMapping, failures);
            Run("waist and foot basis", TestIdentityBasis, failures);
            Run("bent knee keeps flat sole", TestBentKneeFootBasis, failures);
            Run("torso lean keeps flat sole", TestTorsoLeanFootBasis, failures);
            Run("epsilon-safe collapsed limbs", TestCollapsedGeometry, failures);
            Run("quaternion hemisphere continuity", TestHemisphereContinuity, failures);
            Run("camera root remains coherent", TestCameraRootCoherence, failures);
            Run("adaptive rest and floor stabilization", TestAdaptiveRestStabilization, failures);
            Run("fast kick preserves reach and rejects lateral noise", TestFastKickPreservesReach, failures);
            Run("slow stride remains continuous", TestSlowStrideContinuity, failures);
            Run("stabilizer epoch dropout and stale reset", TestStabilizerResetBoundaries, failures);
            Run("stabilizer cadence independence", TestStabilizerCadence, failures);
            Run("left/right 45-degree kicks remain single-rotation", TestObliqueKickBasis, failures);
            Run("raised virtual feet retain body yaw", TestRaisedStrikeFootOrientation, failures);
            Run("planted virtual feet ignore toe yaw", TestPlantedFootOrientation, failures);
            Run("OSC pose-mask contract", TestOscMask, failures);
            Run("3-frame acquire and fallback mux", TestMux, failures);
            return failures.AsReadOnly();
        }

        public static void AssertAll()
        {
            IReadOnlyList<string> failures = RunAll();
            if (failures.Count != 0)
                throw new InvalidOperationException(
                    "Continuous 3D body-pose self-test failed:\n" + string.Join("\n", failures));
        }

        private static void TestImmutableFrames()
        {
            WorldLandmark[] landmarks = CreateLandmarks();
            var original = landmarks[0];
            var frame = new WorldLandmarkFrame(
                1, 10, 3,
                BodyPoseSourceFlags.NativeWorker,
                640, 480, false, landmarks);
            landmarks[0] = default;
            Require(frame[0].IsValid == original.IsValid,
                "WorldLandmarkFrame retained a caller-owned landmark buffer.");

            BodyTrackerPose valid = ValidPose(Vector3.One);
            var trackers = new BodyTrackerPose[BodyTrackerFrame.TrackerSlotCount];
            trackers[0] = valid;
            var trackerFrame = new BodyTrackerFrame(
                1, 10, 3,
                BodyTrackerSource.Legacy2D,
                BodyPoseSourceFlags.Legacy2DReconstruction,
                valid,
                trackers);
            trackers[0] = default;
            Require(trackerFrame.GetTracker(1).PositionValid,
                "BodyTrackerFrame retained a caller-owned tracker buffer.");
        }

        private static void TestCoordinateMapping()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame output = converter.Convert(CreatePoseFrame(1, 100, 7));
            Require(output.CorePoseValid, "Neutral pose did not produce a complete core pose.");

            AssertNear(output.GetTracker(1).Position, new Vector3(0f, 0.9f, 0f), 0.0002f,
                "waist position");
            AssertNear(output.GetTracker(2).Position, new Vector3(-0.2f, 0f, 0f), 0.0002f,
                "left foot position");
            AssertNear(output.GetTracker(3).Position, new Vector3(0.2f, 0f, 0f), 0.0002f,
                "right foot position");
            AssertNear(output.Head.Position, new Vector3(0f, 1.74f, 0f), 0.0002f,
                "synthetic HMD anchor");

            // Rotating the source skeleton +90 degrees about body up must rotate
            // tracker forward the same way, rather than reflecting or swapping it.
            converter.Reset();
            BodyTrackerFrame turned = converter.Convert(CreatePoseFrame(2, 110, 8, yawDegrees: 90f));
            Vector3 actualForward = Vector3.Transform(
                Vector3.UnitZ, turned.GetTracker(1).Orientation);
            Vector3 expectedForward = Vector3.Transform(
                Vector3.UnitZ,
                Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.PI * 0.5f));
            AssertNear(actualForward, expectedForward, 0.0005f, "turned waist forward");
        }

        private static void TestIdentityBasis()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame output = converter.Convert(CreatePoseFrame(1, 100, 4));
            for (int slot = 1; slot <= 3; slot++)
            {
                BodyTrackerPose tracker = output.GetTracker(slot);
                Require(tracker.RotationValid, $"slot {slot} rotation was invalid.");
                Require(IsFinite(tracker.Orientation), $"slot {slot} rotation was non-finite.");
                Require(MathF.Abs(Quaternion.Dot(tracker.Orientation, Quaternion.Identity)) > 0.9999f,
                    $"slot {slot} was not identity for the canonical pose: {tracker.Orientation}.");
            }
        }

        private static void TestCollapsedGeometry()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame good = converter.Convert(CreatePoseFrame(1, 100, 5));
            BodyTrackerFrame collapsedFeet = converter.Convert(
                CreatePoseFrame(2, 110, 5, collapseFootDetail: true));
            for (int slot = 2; slot <= 3; slot++)
            {
                BodyTrackerPose pose = collapsedFeet.GetTracker(slot);
                Require(pose.RotationValid,
                    $"slot {slot} did not use its previous/body-forward foot fallback.");
                Require(IsFinite(pose.Orientation),
                    $"slot {slot} produced NaN for coincident heel/toe points.");
                Require(Quaternion.Dot(good.GetTracker(slot).Orientation, pose.Orientation) > 0.999f,
                    $"slot {slot} flipped during coincident heel/toe fallback.");
            }

            // Foot rotation no longer depends on heel/toe or shin geometry.
            // The virtual target is an ankle position with a flat body-aligned
            // frame, so even missing foot-detail geometry remains usable.
            var fresh = new MediaPipe33TrackerConverter();
            BodyTrackerFrame fullyCollapsed = fresh.Convert(
                CreatePoseFrame(1, 100, 6,
                    collapseFootDetail: true,
                    collapseShins: true));
            for (int slot = 2; slot <= 3; slot++)
            {
                BodyTrackerPose pose = fullyCollapsed.GetTracker(slot);
                Require(pose.RotationValid,
                    $"slot {slot} lost its stable body-aligned rotation with collapsed foot detail.");
                Require(IsFinite(pose.Orientation),
                    $"slot {slot} leaked a non-finite fallback quaternion.");
            }
            Require(fullyCollapsed.CorePoseValid,
                "Valid ankles and torso did not remain eligible when optional foot detail collapsed.");
        }

        private static void TestBentKneeFootBasis()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame output = converter.Convert(
                CreatePoseFrame(1, 100, 12, bentKnees: true));
            for (int slot = 2; slot <= 3; slot++)
            {
                BodyTrackerPose foot = output.GetTracker(slot);
                Require(foot.RotationValid, $"slot {slot} bent-knee foot rotation was invalid.");
                Vector3 soleUp = Vector3.Transform(Vector3.UnitY, foot.Orientation);
                AssertNear(soleUp, Vector3.UnitY, 0.0005f,
                    $"slot {slot} flat sole up with bent knee");
            }
        }

        private static void TestTorsoLeanFootBasis()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame output = converter.Convert(
                CreatePoseFrame(1, 100, 13, sideLeanTorso: true));
            for (int slot = 2; slot <= 3; slot++)
            {
                BodyTrackerPose foot = output.GetTracker(slot);
                Require(foot.RotationValid, $"slot {slot} side-lean foot rotation was invalid.");
                Vector3 soleUp = Vector3.Transform(Vector3.UnitY, foot.Orientation);
                AssertNear(soleUp, Vector3.UnitY, 0.0005f,
                    $"slot {slot} flat sole up with side-leaned torso");
            }
            Require(MathF.Abs(output.Head.Position.X) <= 0.0005f
                && MathF.Abs(output.Head.Position.Z) <= 0.0005f,
                $"side lean moved the synthetic HMD root away from the hips: {output.Head.Position}.");
            Require(MathF.Abs(output.GetTracker(1).Position.X) <= 0.0005f,
                "side lean pulled the waist horizontally away from its hip root.");
        }

        private static void TestHemisphereContinuity()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame a = converter.Convert(
                CreatePoseFrame(1, 100, 9, yawDegrees: 179f));
            BodyTrackerFrame b = converter.Convert(
                CreatePoseFrame(2, 110, 9, yawDegrees: -179f));
            for (int slot = 1; slot <= 3; slot++)
            {
                float dot = Quaternion.Dot(
                    a.GetTracker(slot).Orientation,
                    b.GetTracker(slot).Orientation);
                Require(dot > 0.999f,
                    $"slot {slot} changed quaternion hemisphere at the +/-180 boundary ({dot}).");
            }
        }

        private static void TestCameraRootCoherence()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame baseline = converter.Convert(
                CreatePoseFrame(1, 100, 20));
            BodyTrackerFrame translated = converter.Convert(
                CreatePoseFrame(
                    2,
                    133,
                    20,
                    commonOffset: new Vector3(0.18f, 0.07f, -0.12f)));

            for (int slot = 1; slot <= BodyTrackerFrame.TrackerSlotCount; slot++)
            {
                BodyTrackerPose before = baseline.GetTracker(slot);
                BodyTrackerPose after = translated.GetTracker(slot);
                if (!before.PositionValid || !after.PositionValid)
                    continue;
                AssertNear(
                    after.Position - translated.Head.Position,
                    before.Position - baseline.Head.Position,
                    0.0005f,
                    $"slot {slot} head-relative common-mode position");
            }
        }

        private static void TestAdaptiveRestStabilization()
        {
            var converter = new MediaPipe33TrackerConverter();
            converter.Convert(CreatePoseFrame(1, 100, 21));

            float leftMin = float.PositiveInfinity;
            float leftMax = float.NegativeInfinity;
            float rightMin = float.PositiveInfinity;
            float rightMax = float.NegativeInfinity;
            float waistMin = float.PositiveInfinity;
            float waistMax = float.NegativeInfinity;
            for (int i = 1; i <= 60; i++)
            {
                float sign = (i & 1) == 0 ? 1f : -1f;
                BodyTrackerFrame output = converter.Convert(CreatePoseFrame(
                    i + 1,
                    100 + i * 33,
                    21,
                    commonOffset: new Vector3(sign * 0.008f, 0f, sign * 0.006f),
                    leftLegOffset: new Vector3(sign * 0.020f, sign * 0.010f, 0f),
                    rightLegOffset: new Vector3(-sign * 0.020f, sign * 0.010f, 0f)));
                Require(output.CorePoseValid, "stationary jitter invalidated the core pose.");
                if (i <= 10)
                    continue;

                float leftLocalX = (output.GetTracker(2).Position - output.Head.Position).X;
                float rightLocalX = (output.GetTracker(3).Position - output.Head.Position).X;
                leftMin = MathF.Min(leftMin, leftLocalX);
                leftMax = MathF.Max(leftMax, leftLocalX);
                rightMin = MathF.Min(rightMin, rightLocalX);
                rightMax = MathF.Max(rightMax, rightLocalX);
                waistMin = MathF.Min(waistMin, output.GetTracker(1).Position.Y);
                waistMax = MathF.Max(waistMax, output.GetTracker(1).Position.Y);
            }

            Require(leftMax - leftMin <= 0.015f,
                $"left planted-foot jitter remained {leftMax - leftMin:0.0000} m peak-to-peak.");
            Require(rightMax - rightMin <= 0.015f,
                $"right planted-foot jitter remained {rightMax - rightMin:0.0000} m peak-to-peak.");
            Require(waistMax - waistMin <= 0.006f,
                $"floor noise moved the waist {waistMax - waistMin:0.0000} m peak-to-peak.");
        }

        private static void TestFastKickPreservesReach()
        {
            Vector3 RunKick(bool left)
            {
                var converter = new MediaPipe33TrackerConverter();
                BodyTrackerFrame neutral = converter.Convert(
                    CreatePoseFrame(1, 100, left ? 22u : 23u));
                int slot = left ? 2 : 3;
                Vector3 neutralLocal = neutral.GetTracker(slot).Position - neutral.Head.Position;
                float[] reach = { 0.08f, 0.18f, 0.32f, 0.48f };
                BodyTrackerFrame output = neutral;
                for (int i = 0; i < reach.Length; i++)
                {
                    float lateralNoise = (i & 1) == 0 ? 0.025f : -0.025f;
                    Vector3 motion = new Vector3(
                        lateralNoise,
                        reach[i] * 0.55f,
                        reach[i]);
                    output = converter.Convert(CreatePoseFrame(
                        i + 2,
                        133 + i * 33,
                        left ? 22u : 23u,
                        leftLegOffset: left ? motion : Vector3.Zero,
                        rightLegOffset: left ? Vector3.Zero : motion));
                }

                Vector3 actual = output.GetTracker(slot).Position
                    - output.Head.Position - neutralLocal;
                Require(MathF.Abs(actual.Z - reach[^1]) <= 0.005f,
                    $"{(left ? "left" : "right")} kick reach was {actual.Z:0.000} m, expected {reach[^1]:0.000} m.");
                Require(MathF.Abs(actual.X) <= 0.015f,
                    $"{(left ? "left" : "right")} kick released {actual.X:0.000} m of alternating lateral noise.");
                return actual;
            }

            Vector3 leftKick = RunKick(left: true);
            Vector3 rightKick = RunKick(left: false);
            Require(MathF.Abs(leftKick.Z - rightKick.Z) <= 0.003f,
                $"left/right forward kick response differed: {leftKick.Z} vs {rightKick.Z}.");
        }

        private static void TestSlowStrideContinuity()
        {
            var converter = new MediaPipe33TrackerConverter();
            BodyTrackerFrame output = converter.Convert(CreatePoseFrame(1, 100, 24));
            float previousLeftX = output.GetTracker(2).Position.X;
            float maximumStep = 0f;
            for (int i = 1; i <= 30; i++)
            {
                float outward = 0.004f * i;
                output = converter.Convert(CreatePoseFrame(
                    i + 1,
                    100 + i * 33,
                    24,
                    leftLegOffset: new Vector3(-outward, 0f, 0f),
                    rightLegOffset: new Vector3(outward, 0f, 0f)));
                float leftX = output.GetTracker(2).Position.X;
                maximumStep = MathF.Max(maximumStep, MathF.Abs(leftX - previousLeftX));
                previousLeftX = leftX;
            }

            float expectedLeftX = -0.2f - 30f * 0.004f;
            Require(MathF.Abs(output.GetTracker(2).Position.X - expectedLeftX) <= 0.018f,
                $"slow-stride endpoint lagged by {output.GetTracker(2).Position.X - expectedLeftX:0.0000} m.");
            Require(maximumStep <= 0.009f,
                $"slow stride produced a {maximumStep:0.0000} m lag-then-jut step.");
        }

        private static void TestStabilizerResetBoundaries()
        {
            BodyTrackerFrame Raw(WorldLandmarkFrame frame) =>
                new MediaPipe33TrackerConverter(
                    new MediaPipe33TrackerConverterOptions
                    {
                        EnableAdaptiveStabilization = false,
                    }).Convert(frame);

            var epochConverter = new MediaPipe33TrackerConverter();
            epochConverter.Convert(CreatePoseFrame(1, 100, 25));
            epochConverter.Convert(CreatePoseFrame(
                2, 133, 25, leftLegOffset: new Vector3(0.020f, 0f, 0f)));
            WorldLandmarkFrame newEpochFrame = CreatePoseFrame(
                3, 166, 26, leftLegOffset: new Vector3(-0.020f, 0f, 0f));
            AssertNear(
                epochConverter.Convert(newEpochFrame).GetTracker(2).Position,
                Raw(newEpochFrame).GetTracker(2).Position,
                0.0005f,
                "epoch-reset left foot");

            var dropoutConverter = new MediaPipe33TrackerConverter();
            dropoutConverter.Convert(CreatePoseFrame(1, 100, 27));
            dropoutConverter.Convert(CreatePoseFrame(
                2, 133, 27, leftLegOffset: new Vector3(0.020f, 0f, 0f)));
            BodyTrackerFrame dropout = dropoutConverter.Convert(CreatePoseFrame(
                3, 166, 27, invalidateRightFoot: true));
            Require(!dropout.CorePoseValid, "explicit landmark dropout retained a core pose.");
            WorldLandmarkFrame reacquiredFrame = CreatePoseFrame(
                4, 199, 27, leftLegOffset: new Vector3(-0.020f, 0f, 0f));
            AssertNear(
                dropoutConverter.Convert(reacquiredFrame).GetTracker(2).Position,
                Raw(reacquiredFrame).GetTracker(2).Position,
                0.0005f,
                "dropout-reacquired left foot");

            var boundaryConverter = new MediaPipe33TrackerConverter();
            boundaryConverter.Convert(CreatePoseFrame(1, 100, 28));
            WorldLandmarkFrame boundaryFrame = CreatePoseFrame(
                2, 350, 28, leftLegOffset: new Vector3(0.020f, 0f, 0f));
            float boundaryError = Vector3.Distance(
                boundaryConverter.Convert(boundaryFrame).GetTracker(2).Position,
                Raw(boundaryFrame).GetTracker(2).Position);
            Require(boundaryError >= 0.002f,
                "the exact 250 ms freshness boundary incorrectly reset stabilization.");

            var staleConverter = new MediaPipe33TrackerConverter();
            staleConverter.Convert(CreatePoseFrame(1, 100, 29));
            WorldLandmarkFrame staleFrame = CreatePoseFrame(
                2, 351, 29, leftLegOffset: new Vector3(0.020f, 0f, 0f));
            AssertNear(
                staleConverter.Convert(staleFrame).GetTracker(2).Position,
                Raw(staleFrame).GetTracker(2).Position,
                0.0005f,
                "251 ms stale-gap left foot");
        }

        private static void TestStabilizerCadence()
        {
            float RunCadence(int cadenceMs, uint epoch)
            {
                var converter = new MediaPipe33TrackerConverter();
                converter.Convert(CreatePoseFrame(1, 0, epoch));
                int sequence = 1;
                int previousTimestamp = 0;
                BodyTrackerFrame output = converter.Convert(CreatePoseFrame(2, 1, epoch));
                for (int nominal = cadenceMs; nominal <= 400 + cadenceMs; nominal += cadenceMs)
                {
                    int timestamp = Math.Min(nominal, 400);
                    if (timestamp <= previousTimestamp)
                        break;
                    previousTimestamp = timestamp;
                    float reach = 0.20f * timestamp / 400f;
                    output = converter.Convert(CreatePoseFrame(
                        ++sequence + 1,
                        timestamp,
                        epoch,
                        leftLegOffset: new Vector3(0f, 0f, reach)));
                }
                return output.GetTracker(2).Position.Z;
            }

            float at16 = RunCadence(16, 30);
            float at33 = RunCadence(33, 31);
            float at66 = RunCadence(66, 32);
            float min = MathF.Min(at16, MathF.Min(at33, at66));
            float max = MathF.Max(at16, MathF.Max(at33, at66));
            Require(max - min <= 0.025f,
                $"cadence changed the same stride endpoint by {max - min:0.0000} m ({at16}, {at33}, {at66}).");
        }

        private static void TestObliqueKickBasis()
        {
            void RunSide(bool left, float yawDegrees, uint epoch)
            {
                var converter = new MediaPipe33TrackerConverter();
                BodyTrackerFrame neutral = converter.Convert(
                    CreatePoseFrame(1, 100, epoch, yawDegrees: yawDegrees));
                Vector3 localKick = new Vector3(0f, 0.25f, 0.50f);
                BodyTrackerFrame kick = converter.Convert(CreatePoseFrame(
                    2,
                    133,
                    epoch,
                    yawDegrees: yawDegrees,
                    leftLegOffset: left ? localKick : Vector3.Zero,
                    rightLegOffset: left ? Vector3.Zero : localKick));
                int slot = left ? 2 : 3;
                Vector3 actual = kick.GetTracker(slot).Position
                    - neutral.GetTracker(slot).Position;
                Vector3 expected = Vector3.Transform(
                    localKick,
                    Quaternion.CreateFromAxisAngle(
                        Vector3.UnitY, yawDegrees * MathF.PI / 180f));
                AssertNear(
                    new Vector3(actual.X, 0f, actual.Z),
                    new Vector3(expected.X, 0f, expected.Z),
                    0.006f,
                    $"{(left ? "left" : "right")} single-rotation {yawDegrees:+0;-0}-degree horizontal kick vector");
                Require(actual.Y >= expected.Y * 0.92f,
                    $"{(left ? "left" : "right")} oblique kick lift retained only {actual.Y / expected.Y:P1} of the source motion.");
            }

            RunSide(left: true, yawDegrees: 45f, epoch: 33);
            RunSide(left: false, yawDegrees: -45f, epoch: 34);
        }

        private static void TestRaisedStrikeFootOrientation()
        {
            void RunSide(
                bool left,
                float bodyYawDegrees,
                float hallucinatedFootYawDegrees,
                uint epoch)
            {
                var converter = new MediaPipe33TrackerConverter();
                converter.Convert(CreatePoseFrame(
                    1, 100, epoch, yawDegrees: bodyYawDegrees));

                Vector3 kick = new Vector3(0f, 0.30f, 0.45f);
                Vector3 knee = 0.5f * kick;
                BodyTrackerFrame output = converter.Convert(CreatePoseFrame(
                    2,
                    133,
                    epoch,
                    yawDegrees: bodyYawDegrees,
                    leftLegOffset: left ? kick : Vector3.Zero,
                    rightLegOffset: left ? Vector3.Zero : kick,
                    leftKneeOffset: left ? knee : null,
                    rightKneeOffset: left ? null : knee,
                    leftFootYawErrorDegrees:
                        left ? hallucinatedFootYawDegrees : 0f,
                    rightFootYawErrorDegrees:
                        left ? 0f : hallucinatedFootYawDegrees));

                int slot = left ? 2 : 3;
                float correctedYaw = ForwardYawDegrees(
                    output.GetTracker(slot).Orientation);
                Require(MathF.Abs(correctedYaw - bodyYawDegrees) <= 0.1f,
                    $"{(left ? "left" : "right")} raised foot yaw was {correctedYaw:0.0}, expected body yaw {bodyYawDegrees:0.0} degrees.");
            }

            RunSide(left: true, bodyYawDegrees: 0f,
                hallucinatedFootYawDegrees: 90f, epoch: 35);
            RunSide(left: false, bodyYawDegrees: 0f,
                hallucinatedFootYawDegrees: -90f, epoch: 36);
            RunSide(left: true, bodyYawDegrees: 45f,
                hallucinatedFootYawDegrees: -75f, epoch: 39);
            RunSide(left: false, bodyYawDegrees: -45f,
                hallucinatedFootYawDegrees: 75f, epoch: 40);
        }

        private static void TestPlantedFootOrientation()
        {
            var converter = new MediaPipe33TrackerConverter();
            converter.Convert(CreatePoseFrame(1, 100, 37));

            // Neither one-frame nor sustained toe hallucinations may rotate
            // the virtual tracker, its saved calibration offset, or the FBT
            // knee pole.
            BodyTrackerFrame spike = converter.Convert(CreatePoseFrame(
                2,
                133,
                37,
                rightFootYawErrorDegrees: 90f));
            float spikeYaw = ForwardYawDegrees(spike.GetTracker(3).Orientation);
            Require(MathF.Abs(spikeYaw) <= 0.1f,
                $"one-frame planted right-foot yaw changed to {spikeYaw:0.0} degrees.");

            BodyTrackerFrame settled = spike;
            for (int i = 0; i < 12; i++)
            {
                settled = converter.Convert(CreatePoseFrame(
                    i + 3,
                    166 + i * 33,
                    37,
                    rightFootYawErrorDegrees: 30f));
            }
            float settledYaw = ForwardYawDegrees(settled.GetTracker(3).Orientation);
            Require(MathF.Abs(settledYaw) <= 0.1f,
                $"sustained planted toe yaw rotated the virtual tracker to {settledYaw:0.0} degrees.");
        }

        private static void TestOscMask()
        {
            BodyTrackerFrame frame = CreateCoreTrackerFrame(
                1, 100, 1, BodyTrackerSource.Continuous3D);
            Require(frame.OscPoseMask == 0x00030707u,
                $"pose mask was 0x{frame.OscPoseMask:x}, expected 0x30707.");

            OscTrackerFrameFlags flags = OscTrackerFrameContract.BuildPolicyFlags(
                frame, followHmdYaw: true, allowGaitFootRelease: false);
            OscTrackerFrameFlags expected = OscTrackerFrameFlags.UseHeadTranslation
                | OscTrackerFrameFlags.UseHeadHeightScale
                | OscTrackerFrameFlags.UseHeadYaw
                | OscTrackerFrameFlags.FollowHmdYaw
                | OscTrackerFrameFlags.Continuous3D;
            Require(flags == expected, $"OSC policy flags were {flags}, expected {expected}.");

            BodyTrackerFrame mediaPipeFrame = new MediaPipe33TrackerConverter()
                .Convert(CreatePoseFrame(1, 100, 38));
            OscTrackerFrameFlags mediaPipeFlags =
                OscTrackerFrameContract.BuildPolicyFlags(
                    mediaPipeFrame,
                    followHmdYaw: true,
                    allowGaitFootRelease: true);
            OscTrackerFrameFlags expectedMediaPipe =
                OscTrackerFrameFlags.UseHeadTranslation
                | OscTrackerFrameFlags.UseHeadHeightScale
                | OscTrackerFrameFlags.UseHeadYaw
                | OscTrackerFrameFlags.FollowHmdYaw
                | OscTrackerFrameFlags.AllowGaitFootRelease
                | OscTrackerFrameFlags.Continuous3D
                | OscTrackerFrameFlags.HeadXZIsBodyRoot;
            Require(mediaPipeFlags == expectedMediaPipe,
                $"MediaPipe OSC policy flags were {mediaPipeFlags}, expected {expectedMediaPipe}.");

            Vector3 packed = OscTrackerFrameContract.PackHeader(17, frame.OscPoseMask, flags);
            Require(packed.X == 17f
                && packed.Y == frame.OscPoseMask
                && packed.Z == (uint)flags,
                "OSC header integers did not survive float packing exactly.");
        }

        private static void TestMux()
        {
            var mux = new BodyTrackerFrameMux();
            BodyTrackerFrame fallback0 = CreateCoreTrackerFrame(
                1, 0, 10, BodyTrackerSource.Legacy2D);
            BodyTrackerFrame primary1 = CreateCoreTrackerFrame(
                1, 0, 20, BodyTrackerSource.Continuous3D);

            Require(mux.TrySelect(0, primary1, fallback0, out BodyTrackerMuxSelection selected),
                "mux did not select its valid fallback.");
            Require(selected.Source == BodyTrackerSource.Legacy2D,
                "continuous 3D acquired before three frames.");

            // Re-reading the same worker snapshot must not increase the streak.
            Require(mux.TrySelect(1, primary1,
                CreateCoreTrackerFrame(2, 1, 10, BodyTrackerSource.Legacy2D), out selected),
                "mux lost fallback while primary repeated.");
            Require(mux.PrimaryAcquireProgress == 1,
                "a repeated primary frame counted toward acquisition.");

            BodyTrackerFrame primary2 = CreateCoreTrackerFrame(
                2, 10, 20, BodyTrackerSource.Continuous3D);
            mux.TrySelect(10, primary2,
                CreateCoreTrackerFrame(3, 10, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Legacy2D,
                "continuous 3D acquired after only two distinct frames.");

            BodyTrackerFrame primary3 = CreateCoreTrackerFrame(
                3, 20, 20, BodyTrackerSource.Continuous3D);
            mux.TrySelect(20, primary3,
                CreateCoreTrackerFrame(4, 20, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Continuous3D,
                "continuous 3D did not acquire on its third valid frame.");
            uint primaryOutputEpoch = selected.OutputSourceEpoch;

            // No newer inference holds the last good primary through exactly
            // 250 ms, then falls back on the next millisecond.
            mux.TrySelect(270, null,
                CreateCoreTrackerFrame(5, 270, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Continuous3D,
                "primary fell back before the 250 ms freshness boundary.");
            mux.TrySelect(271, null,
                CreateCoreTrackerFrame(6, 271, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Legacy2D && selected.SourceChanged,
                "stale primary did not immediately switch to fallback after 250 ms.");
            Require(selected.OutputSourceEpoch != primaryOutputEpoch,
                "source transition did not advance the OSC epoch.");

            // Reacquire, then prove one explicitly invalid inference is held
            // through the same 250 ms grace instead of resetting alignment on
            // a one-frame model miss.
            mux.TrySelect(280,
                CreateCoreTrackerFrame(4, 280, 20, BodyTrackerSource.Continuous3D),
                CreateCoreTrackerFrame(7, 280, 10, BodyTrackerSource.Legacy2D), out selected);
            mux.TrySelect(290,
                CreateCoreTrackerFrame(5, 290, 20, BodyTrackerSource.Continuous3D),
                CreateCoreTrackerFrame(8, 290, 10, BodyTrackerSource.Legacy2D), out selected);
            mux.TrySelect(300,
                CreateCoreTrackerFrame(6, 300, 20, BodyTrackerSource.Continuous3D),
                CreateCoreTrackerFrame(9, 300, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Continuous3D,
                "primary did not reacquire after three new frames.");

            var invalidTrackers = new BodyTrackerPose[BodyTrackerFrame.TrackerSlotCount];
            var invalidPrimary = new BodyTrackerFrame(
                7, 301, 20,
                BodyTrackerSource.Continuous3D,
                BodyPoseSourceFlags.Continuous3D,
                BodyTrackerPose.Invalid,
                invalidTrackers);
            mux.TrySelect(301, invalidPrimary,
                CreateCoreTrackerFrame(10, 301, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Continuous3D
                && !selected.SourceChanged,
                "one invalid inference reset the acquired primary immediately.");
            mux.TrySelect(550, null,
                CreateCoreTrackerFrame(11, 550, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Continuous3D,
                "invalid inference grace ended before 250 ms.");
            mux.TrySelect(551, null,
                CreateCoreTrackerFrame(12, 551, 10, BodyTrackerSource.Legacy2D), out selected);
            Require(selected.Source == BodyTrackerSource.Legacy2D && selected.SourceChanged,
                "invalid primary did not yield after its last good frame became stale.");

            // A worker epoch is different: its camera origin may have changed,
            // so it must never inherit the previous epoch's acquired state.
            var epochMux = new BodyTrackerFrameMux();
            for (int i = 1; i <= 3; i++)
            {
                epochMux.TrySelect(i * 10,
                    CreateCoreTrackerFrame(i, i * 10, 40,
                        BodyTrackerSource.Continuous3D),
                    CreateCoreTrackerFrame(i, i * 10, 10,
                        BodyTrackerSource.Legacy2D),
                    out selected);
            }
            Require(selected.Source == BodyTrackerSource.Continuous3D,
                "epoch test primary did not acquire.");
            epochMux.TrySelect(40,
                CreateCoreTrackerFrame(1, 40, 41,
                    BodyTrackerSource.Continuous3D),
                CreateCoreTrackerFrame(4, 40, 10,
                    BodyTrackerSource.Legacy2D),
                out selected);
            Require(selected.Source == BodyTrackerSource.Legacy2D
                && epochMux.PrimaryAcquireProgress == 1,
                "new source epoch inherited the old primary acquisition.");
        }

        private static WorldLandmarkFrame CreatePoseFrame(
            long sequence,
            long timestamp,
            uint epoch,
            float yawDegrees = 0f,
            bool collapseFootDetail = false,
            bool collapseShins = false,
            bool bentKnees = false,
            bool sideLeanTorso = false,
            Vector3 commonOffset = default,
            Vector3 torsoOffset = default,
            Vector3 leftLegOffset = default,
            Vector3 rightLegOffset = default,
            Vector3? leftKneeOffset = null,
            Vector3? rightKneeOffset = null,
            float leftFootYawErrorDegrees = 0f,
            float rightFootYawErrorDegrees = 0f,
            bool invalidateRightFoot = false)
        {
            WorldLandmark[] landmarks = CreateLandmarks();
            Quaternion yaw = Quaternion.CreateFromAxisAngle(
                Vector3.UnitY, yawDegrees * MathF.PI / 180f);

            void Put(MediaPipe33LandmarkIndex index, Vector3 ocuPoint)
            {
                ocuPoint += commonOffset;
                ocuPoint = Vector3.Transform(ocuPoint, yaw);
                // Inverse of the converter's unmirrored MediaPipe mapping.
                Vector3 raw = new Vector3(ocuPoint.X, -ocuPoint.Y, -ocuPoint.Z);
                landmarks[(int)index] = new WorldLandmark(
                    new Vector3(0.5f, 0.5f, 0f), raw, 1f);
            }

            Put(MediaPipe33LandmarkIndex.LeftHip,
                new Vector3(-0.2f, 0f, 0f) + torsoOffset);
            Put(MediaPipe33LandmarkIndex.RightHip,
                new Vector3(0.2f, 0f, 0f) + torsoOffset);
            float shoulderLeanX = sideLeanTorso ? 0.30f : 0f;
            Put(MediaPipe33LandmarkIndex.LeftShoulder,
                new Vector3(-0.25f + shoulderLeanX, 0.6f, 0f) + torsoOffset);
            Put(MediaPipe33LandmarkIndex.RightShoulder,
                new Vector3(0.25f + shoulderLeanX, 0.6f, 0f) + torsoOffset);
            Put(MediaPipe33LandmarkIndex.LeftElbow,
                new Vector3(-0.45f, 0.35f, 0f) + torsoOffset);
            Put(MediaPipe33LandmarkIndex.RightElbow,
                new Vector3(0.45f, 0.35f, 0f) + torsoOffset);

            Vector3 leftAnkle = new Vector3(-0.2f, -0.9f, 0f) + leftLegOffset;
            Vector3 rightAnkle = new Vector3(0.2f, -0.9f, 0f) + rightLegOffset;
            Put(MediaPipe33LandmarkIndex.LeftAnkle, leftAnkle);
            Put(MediaPipe33LandmarkIndex.RightAnkle, rightAnkle);
            Put(MediaPipe33LandmarkIndex.LeftKnee,
                collapseShins ? leftAnkle
                    : new Vector3(-0.2f, -0.45f, bentKnees ? 0.30f : 0f)
                        + (leftKneeOffset ?? leftLegOffset));
            Put(MediaPipe33LandmarkIndex.RightKnee,
                collapseShins ? rightAnkle
                    : new Vector3(0.2f, -0.45f, bentKnees ? 0.30f : 0f)
                        + (rightKneeOffset ?? rightLegOffset));

            Vector3 FootDetail(Vector3 ankle, float localZ, float localYawDegrees)
            {
                Vector3 local = Vector3.Transform(
                    new Vector3(0f, 0f, localZ),
                    Quaternion.CreateFromAxisAngle(
                        Vector3.UnitY,
                        localYawDegrees * MathF.PI / 180f));
                return ankle + local;
            }

            Put(MediaPipe33LandmarkIndex.LeftHeel,
                collapseFootDetail ? leftAnkle
                    : FootDetail(leftAnkle, -0.1f, leftFootYawErrorDegrees));
            Put(MediaPipe33LandmarkIndex.RightHeel,
                collapseFootDetail ? rightAnkle
                    : FootDetail(rightAnkle, -0.1f, rightFootYawErrorDegrees));
            Put(MediaPipe33LandmarkIndex.LeftFootIndex,
                collapseFootDetail ? leftAnkle
                    : FootDetail(leftAnkle, 0.15f, leftFootYawErrorDegrees));
            Put(MediaPipe33LandmarkIndex.RightFootIndex,
                collapseFootDetail ? rightAnkle
                    : FootDetail(rightAnkle, 0.15f, rightFootYawErrorDegrees));

            if (invalidateRightFoot)
            {
                landmarks[(int)MediaPipe33LandmarkIndex.RightAnkle] = default;
                landmarks[(int)MediaPipe33LandmarkIndex.RightHeel] = default;
                landmarks[(int)MediaPipe33LandmarkIndex.RightFootIndex] = default;
            }

            return new WorldLandmarkFrame(
                sequence,
                timestamp,
                epoch,
                BodyPoseSourceFlags.NativeWorker,
                640,
                480,
                inputMirrored: false,
                landmarks);
        }

        private static WorldLandmark[] CreateLandmarks()
        {
            var landmarks = new WorldLandmark[WorldLandmarkFrame.MediaPipeLandmarkCount];
            // A valid sentinel proves constructor copying without accidentally
            // making an unused anatomical point eligible for conversion.
            landmarks[0] = new WorldLandmark(
                Vector3.Zero, Vector3.Zero, 0f, isValid: true);
            return landmarks;
        }

        private static BodyTrackerFrame CreateCoreTrackerFrame(
            long sequence,
            long timestamp,
            uint epoch,
            BodyTrackerSource source)
        {
            BodyTrackerPose valid = ValidPose(Vector3.Zero);
            var trackers = new BodyTrackerPose[BodyTrackerFrame.TrackerSlotCount];
            trackers[0] = valid;
            trackers[1] = valid;
            trackers[2] = valid;
            BodyPoseSourceFlags flags = source == BodyTrackerSource.Continuous3D
                ? BodyPoseSourceFlags.Continuous3D
                : BodyPoseSourceFlags.Legacy2DReconstruction;
            return new BodyTrackerFrame(
                sequence, timestamp, epoch, source, flags, valid, trackers);
        }

        private static BodyTrackerPose ValidPose(Vector3 position) =>
            new BodyTrackerPose(
                position,
                Quaternion.Identity,
                1f,
                positionValid: true,
                rotationValid: true);

        private static bool IsFinite(Quaternion value) =>
            float.IsFinite(value.X)
            && float.IsFinite(value.Y)
            && float.IsFinite(value.Z)
            && float.IsFinite(value.W);

        private static float ForwardYawDegrees(Quaternion orientation)
        {
            Vector3 forward = Vector3.Transform(Vector3.UnitZ, orientation);
            return MathF.Atan2(forward.X, forward.Z) * 180f / MathF.PI;
        }

        private static void AssertNear(
            Vector3 actual,
            Vector3 expected,
            float tolerance,
            string label)
        {
            float error = Vector3.Distance(actual, expected);
            Require(error <= tolerance,
                $"{label}: got {actual}, expected {expected}, error {error}.");
        }

        private static void Require(bool condition, string message)
        {
            if (!condition)
                throw new InvalidOperationException(message);
        }

        private static void Run(
            string name,
            Action test,
            List<string> failures)
        {
            try
            {
                test();
            }
            catch (Exception exception)
            {
                failures.Add($"{name}: {exception.Message}");
            }
        }
    }
}
