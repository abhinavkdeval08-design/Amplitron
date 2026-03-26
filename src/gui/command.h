#pragma once

#include "common.h"
#include "audio/audio_engine.h"
#include "audio/effect.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace GuitarAmp {

/**
 * @brief Abstract base class for all undoable commands (Gang of Four Command Pattern).
 *
 * Each concrete command encapsulates a single reversible action on the audio
 * engine (e.g. adding an effect, changing a parameter). Commands are stored
 * in a CommandHistory and invoked via execute() / undo().
 */
class Command {
public:
    virtual ~Command() = default;

    /** @brief Apply this command's action. */
    virtual void execute() = 0;

    /** @brief Reverse this command's action. */
    virtual void undo() = 0;

    /** @brief Return a short human-readable label (shown in the Edit menu). */
    virtual const char* description() const = 0;

    /**
     * @brief Attempt to merge @p other into this command (coalescing).
     *
     * Two commands can merge if they affect the same target within a short
     * time window. Returns true if this command absorbed @p other.
     */
    virtual bool merge_with(const Command& /*other*/) { return false; }

    /** @brief Return the steady-clock time point when this command was created. */
    auto timestamp() const { return timestamp_; }

protected:
    std::chrono::steady_clock::time_point timestamp_ = std::chrono::steady_clock::now();
};

/**
 * @brief Command that appends an effect to the engine's signal chain.
 *
 * execute() adds the effect; undo() finds and removes it.
 */
class AddEffectCommand : public Command {
public:
    /**
     * @brief Construct an AddEffectCommand.
     * @param engine  Reference to the audio engine that owns the effect chain.
     * @param effect  Shared pointer to the effect to add.
     */
    AddEffectCommand(AudioEngine& engine, std::shared_ptr<Effect> effect)
        : engine_(engine), effect_(std::move(effect)) {}

    /** @brief Append the effect to the engine's chain (before the amp if present). */
    void execute() override {
        int amp_idx = -1;
        auto& fx = engine_.effects();
        for (int i = 0; i < static_cast<int>(fx.size()); ++i) {
            if (std::strcmp(fx[i]->name(), "Amp Sim") == 0) {
                amp_idx = i;
                break;
            }
        }
        if (amp_idx >= 0) {
            engine_.insert_effect(amp_idx, effect_);
        } else {
            engine_.add_effect(effect_);
        }
    }

    /** @brief Remove the previously added effect from the chain. */
    void undo() override {
        auto& fx = engine_.effects();
        for (int i = static_cast<int>(fx.size()) - 1; i >= 0; --i) {
            if (fx[i] == effect_) {
                engine_.remove_effect(i);
                return;
            }
        }
    }

    /** @brief Return "Add Effect". */
    const char* description() const override { return "Add Effect"; }

    /** @brief Accessor for the wrapped effect. */
    std::shared_ptr<Effect> effect() const { return effect_; }

private:
    AudioEngine& engine_;
    std::shared_ptr<Effect> effect_;
};

/**
 * @brief Command that removes an effect from the engine's signal chain.
 *
 * The constructor captures the effect pointer before removal so undo() can
 * re-insert it at its original position.
 */
class RemoveEffectCommand : public Command {
public:
    /**
     * @brief Construct a RemoveEffectCommand.
     * @param engine  Reference to the audio engine.
     * @param index   Chain index of the effect to remove (captured at construction).
     */
    RemoveEffectCommand(AudioEngine& engine, int index)
        : engine_(engine), index_(index) {
        auto& fx = engine_.effects();
        if (index >= 0 && index < static_cast<int>(fx.size())) {
            effect_ = fx[index];
        }
    }

    /** @brief Remove the effect at the stored index. */
    void execute() override {
        engine_.remove_effect(index_);
    }

    /** @brief Re-insert the captured effect at its original chain position. */
    void undo() override {
        if (effect_) {
            auto& fx = engine_.effects();
            int pos = std::min(index_, static_cast<int>(fx.size()));
            engine_.add_effect(effect_);
            int last = static_cast<int>(engine_.effects().size()) - 1;
            if (last != pos) {
                engine_.move_effect(last, pos);
            }
        }
    }

    /** @brief Return "Remove Effect". */
    const char* description() const override { return "Remove Effect"; }

    /** @brief Original chain index of the removed effect. */
    int index() const { return index_; }

    /** @brief Accessor for the captured effect pointer. */
    std::shared_ptr<Effect> effect() const { return effect_; }

private:
    AudioEngine& engine_;
    int index_;
    std::shared_ptr<Effect> effect_;
};

/**
 * @brief Command that moves an effect from one chain position to another.
 *
 * execute() calls move_effect(from, to); undo() reverses the move.
 */
class ReorderEffectCommand : public Command {
public:
    /**
     * @brief Construct a ReorderEffectCommand.
     * @param engine  Reference to the audio engine.
     * @param from    Source index in the effect chain.
     * @param to      Destination index in the effect chain.
     */
    ReorderEffectCommand(AudioEngine& engine, int from, int to)
        : engine_(engine), from_(from), to_(to) {}

    /** @brief Move the effect from source to destination index. */
    void execute() override {
        engine_.move_effect(from_, to_);
    }

    /** @brief Move the effect back from destination to source index. */
    void undo() override {
        engine_.move_effect(to_, from_);
    }

    /** @brief Return "Reorder Effect". */
    const char* description() const override { return "Reorder Effect"; }

    /** @brief Original source index. */
    int from() const { return from_; }

    /** @brief Destination index. */
    int to() const { return to_; }

private:
    AudioEngine& engine_;
    int from_;
    int to_;
};

/**
 * @brief Command that records a single parameter value change on an effect.
 *
 * Supports coalescing: rapid changes to the same parameter within 500 ms are
 * merged into one undo step. Uses shared_ptr<Effect> directly so it is robust
 * against effect chain reordering.
 */
class ParameterChangeCommand : public Command {
public:
    /**
     * @brief Construct a ParameterChangeCommand.
     * @param engine      Reference to the AudioEngine.
     * @param effect      Shared pointer to the target effect.
     * @param param_index Index of the parameter within the effect's param list.
     * @param old_value   Value before the change (used by undo).
     * @param new_value   Value after the change (used by execute).
     */
    ParameterChangeCommand(AudioEngine& engine, std::shared_ptr<Effect> effect,
                           int param_index, float old_value, float new_value)
        : engine_(engine), effect_(std::move(effect)),
          param_index_(param_index), old_value_(old_value), new_value_(new_value) {}

    /** @brief Set the parameter to new_value_. */
    void execute() override {
        auto& params = effect_->params();
        if (param_index_ >= 0 && param_index_ < static_cast<int>(params.size())) {
            params[param_index_].value = new_value_;
            int idx = -1;
            auto& fx = engine_.effects();
            for (int i = 0; i < static_cast<int>(fx.size()); ++i) {
                if (fx[i] == effect_) { idx = i; break; }
            }
            if (idx >= 0) engine_.push_param_change(idx, param_index_, new_value_);
        }
    }

    /** @brief Restore the parameter to old_value_. */
    void undo() override {
        auto& params = effect_->params();
        if (param_index_ >= 0 && param_index_ < static_cast<int>(params.size())) {
            params[param_index_].value = old_value_;
            int idx = -1;
            auto& fx = engine_.effects();
            for (int i = 0; i < static_cast<int>(fx.size()); ++i) {
                if (fx[i] == effect_) { idx = i; break; }
            }
            if (idx >= 0) engine_.push_param_change(idx, param_index_, old_value_);
        }
    }

    /** @brief Return "Change Parameter". */
    const char* description() const override { return "Change Parameter"; }

    /**
     * @brief Attempt to coalesce @p other into this command.
     *
     * Merges if @p other is a ParameterChangeCommand targeting the same
     * effect and parameter index within 500 ms. On success, this command's
     * new_value_ and timestamp are updated; old_value_ is preserved.
     *
     * @return true if @p other was absorbed.
     */
    bool merge_with(const Command& other) override {
        auto* pc = dynamic_cast<const ParameterChangeCommand*>(&other);
        if (!pc) return false;
        if (pc->effect_.get() != effect_.get() || pc->param_index_ != param_index_)
            return false;

        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            pc->timestamp_ - timestamp_);
        if (dt.count() > 500) return false;

        new_value_ = pc->new_value_;
        timestamp_ = pc->timestamp_;
        return true;
    }

    /** @brief Accessor for the target effect. */
    std::shared_ptr<Effect> effect() const { return effect_; }

    /** @brief Index of the parameter being changed. */
    int param_index() const { return param_index_; }

    /** @brief Value before the change. */
    float old_value() const { return old_value_; }

    /** @brief Value after the change. */
    float new_value() const { return new_value_; }

private:
    AudioEngine& engine_;
    std::shared_ptr<Effect> effect_;
    int param_index_;
    float old_value_;
    float new_value_;
};

/**
 * @brief Command that captures the full effect-chain state before and after a
 *        preset load, enabling undo/redo of the entire preset switch.
 *
 * Uses AudioEngine::restore_effects_state() so the effect chain is replaced
 * atomically under the engine's mutex.
 */
class LoadPresetCommand : public Command {
public:
    /**
     * @brief Snapshot of a single effect's configuration at a point in time.
     */
    struct EffectSnapshot {
        std::shared_ptr<Effect> effect;   ///< The effect instance.
        bool enabled;                     ///< Whether the effect was enabled.
        float mix;                        ///< Dry/wet mix level.
        std::vector<float> param_values;  ///< Ordered parameter values.
    };

    /**
     * @brief Construct a LoadPresetCommand.
     * @param engine             Reference to the audio engine.
     * @param before_state       Effect chain snapshot before the load.
     * @param before_input_gain  Input gain before the load.
     * @param before_output_gain Output gain before the load.
     * @param after_state        Effect chain snapshot after the load.
     * @param after_input_gain   Input gain after the load.
     * @param after_output_gain  Output gain after the load.
     */
    LoadPresetCommand(AudioEngine& engine,
                      std::vector<EffectSnapshot> before_state,
                      float before_input_gain, float before_output_gain,
                      std::vector<EffectSnapshot> after_state,
                      float after_input_gain, float after_output_gain)
        : engine_(engine),
          before_state_(std::move(before_state)),
          before_input_gain_(before_input_gain),
          before_output_gain_(before_output_gain),
          after_state_(std::move(after_state)),
          after_input_gain_(after_input_gain),
          after_output_gain_(after_output_gain) {}

    /** @brief Restore the after-load state (redo). */
    void execute() override {
        apply_state(after_state_, after_input_gain_, after_output_gain_);
    }

    /** @brief Restore the before-load state (undo). */
    void undo() override {
        apply_state(before_state_, before_input_gain_, before_output_gain_);
    }

    /** @brief Return "Load Preset". */
    const char* description() const override { return "Load Preset"; }

private:
    /**
     * @brief Replace the engine's effect chain and gains with the given snapshot.
     * @param state        Vector of EffectSnapshot to restore.
     * @param input_gain   Input gain to set.
     * @param output_gain  Output gain to set.
     */
    void apply_state(const std::vector<EffectSnapshot>& state,
                     float input_gain, float output_gain) {
        // Prepare effect list with restored params before swapping
        std::vector<std::shared_ptr<Effect>> new_effects;
        new_effects.reserve(state.size());
        for (auto& snap : state) {
            snap.effect->set_enabled(snap.enabled);
            snap.effect->set_mix(snap.mix);
            auto& params = snap.effect->params();
            for (int i = 0; i < static_cast<int>(params.size()) &&
                            i < static_cast<int>(snap.param_values.size()); ++i) {
                params[i].value = snap.param_values[i];
            }
            new_effects.push_back(snap.effect);
        }

        // Atomic swap under engine lock — audio thread never sees half-applied state
        engine_.restore_effects_state(std::move(new_effects));

        engine_.set_input_gain(input_gain);
        engine_.set_output_gain(output_gain);
    }

    AudioEngine& engine_;
    std::vector<EffectSnapshot> before_state_;
    float before_input_gain_;
    float before_output_gain_;
    std::vector<EffectSnapshot> after_state_;
    float after_input_gain_;
    float after_output_gain_;
};

/**
 * @brief Command that removes every effect from the signal chain at once.
 *
 * Captures the full chain state before clearing so that undo() can restore it.
 */
class ClearAllCommand : public Command {
public:
    explicit ClearAllCommand(AudioEngine& engine)
        : engine_(engine) {
        for (auto& fx : engine_.effects()) {
            saved_.push_back(fx);
        }
    }

    void execute() override {
        while (!engine_.effects().empty()) {
            engine_.remove_effect(static_cast<int>(engine_.effects().size()) - 1);
        }
    }

    void undo() override {
        for (auto& fx : saved_) {
            engine_.add_effect(fx);
        }
    }

    const char* description() const override { return "Clear All"; }

private:
    AudioEngine& engine_;
    std::vector<std::shared_ptr<Effect>> saved_;
};

} // namespace GuitarAmp
