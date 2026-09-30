#pragma once

#include <cstdint>

namespace ocu
{
    // Game-thread state for a confirmed naming request or a desktop name field.
    // VR normally keeps textEntry disabled, even after accepting the confirmation.
    class RaceMenuKeyboardRecovery
    {
    public:
        void ConfirmAccepted(std::uintptr_t movie, std::uint64_t now)
        {
            *this = {};
            movie_ = movie;
            confirmed_ = movie != 0;
            active_ = confirmed_;
            enteredAt_ = now;
        }

        void NativeRequest(std::uintptr_t movie)
        {
            // Skyrim VR only arms naming here. Its Accept-button release
            // actually opens the overlay, so this is not completion evidence.
            if (movie && movie == movie_)
                nativeRequested_ = true;
        }

        bool NativeRequestObserved() const { return nativeRequested_; }

        bool Observe(std::uintptr_t movie, bool textEntryActive, bool keyboardBusy,
            bool triggerHeld, std::uint64_t now)
        {
            if (movie != movie_) {
                *this = {};
                movie_ = movie;
            }
            if (!textEntryActive && !confirmed_) {
                active_ = false;
                consumed_ = false;
                nativeRequested_ = false;
            }
            if (!movie || (!textEntryActive && !confirmed_))
                return false;
            if (!active_) {
                active_ = true;
                enteredAt_ = now;
            }
            // Never resurrect a stale confirmation after a long modal interruption.
            if (keyboardBusy || (confirmed_ && now - enteredAt_ > 5000))
                consumed_ = true;
            // Allow the normal native request to run first, and require release
            // of the confirmation trigger before opening a clickable keyboard.
            if (consumed_ || triggerHeld || now - enteredAt_ < 150)
                return false;
            consumed_ = true;
            return true;
        }

    private:
        std::uintptr_t movie_ = 0;
        std::uint64_t enteredAt_ = 0;
        bool active_ = false;
        bool consumed_ = false;
        bool confirmed_ = false;
        bool nativeRequested_ = false;
    };
}
