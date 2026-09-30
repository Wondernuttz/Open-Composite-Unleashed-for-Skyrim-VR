// MCM's options are dynamically rendered BasicList rows, not native buttons.
// Keep this adapter confined to the live ConfigPanel and use its own callbacks.
struct MCMLaserTarget
{
	RE::GFxValue list;
	RE::GFxValue clip;
	int index = -1;
};

bool GetReadyMCMPanel(RE::GFxMovieView& movie, RE::GFxValue& panel)
{
	if (JournalMainFaderIsInteractive(movie) ||
	    !movie.GetVariable(&panel, "_root.ConfigPanelFader.configPanel") ||
	    (!panel.IsObject() && !panel.IsDisplayObject()) || !DisplayObjectIsUsable(panel))
		return false;
	RE::GFxValue state;
	RE::GFxValue remapping;
	return panel.GetMember("_state", &state) && state.IsNumber() && state.GetNumber() == 0 &&
	    !(panel.GetMember("_bRemapMode", &remapping) && remapping.IsBool() && remapping.GetBool());
}

bool MCMListAcceptsInput(RE::GFxValue& list)
{
	if ((!list.IsObject() && !list.IsDisplayObject()) || !DisplayObjectIsUsable(list))
		return false;
	for (const char* name : { "disableInput", "disableSelection", "isListAnimating" }) {
		RE::GFxValue value;
		if (list.GetMember(name, &value) && value.IsBool() && value.GetBool())
			return false;
	}
	return true;
}

bool ResolveMCMOptionTarget(RE::GFxMovieView& movie, float viewportX,
    float viewportY, MCMLaserTarget& target, bool* optionsArea = nullptr)
{
	target = {};
	if (optionsArea)
		*optionsArea = false;
	RE::GFxValue panel, list, background, count;
	if (!GetReadyMCMPanel(movie, panel) || !panel.GetMember("_optionsList", &list) ||
	    !MCMListAcceptsInput(list) || !list.GetMember("background", &background) ||
	    !list.GetMember("_listIndex", &count) || !count.IsNumber())
		return false;
	const double rendered = count.GetNumber();
	if (!std::isfinite(rendered) || rendered < 1 || rendered > 256)
		return false;
	float rootX = 0, rootY = 0;
	if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY) ||
	    !DisplayObjectBoundsHitAtRootPoint(movie, background, rootX, rootY))
		return false;
	if (optionsArea)
		*optionsArea = true;
	// Inspect only currently rendered clips. Their itemIndex already accounts for
	// scrolling and both columns; row arithmetic would break custom layouts.
	for (int i = 0; i < static_cast<int>(rendered); ++i) {
		RE::GFxValue arg, clip, index, enabled, rowBackground;
		arg.SetNumber(i);
		if (!list.Invoke("getClipByIndex", &clip, &arg, 1) ||
		    (!clip.IsObject() && !clip.IsDisplayObject()) || !DisplayObjectIsUsable(clip) ||
		    !clip.GetMember("enabled", &enabled) || !enabled.IsBool() || !enabled.GetBool() ||
		    !clip.GetMember("itemIndex", &index) || !index.IsNumber() ||
		    !std::isfinite(index.GetNumber()) || index.GetNumber() < 0 || index.GetNumber() > 65535 ||
		    !clip.GetMember("background", &rowBackground) ||
		    !DisplayObjectBoundsHitAtRootPoint(movie, rowBackground, rootX, rootY))
			continue;
		target.list = list;
		target.clip = clip;
		target.index = static_cast<int>(index.GetNumber());
		return true;
	}
	return false;
}

bool HoverMCMOption(MCMLaserTarget& target)
{
	if (target.index < 0 || !MCMListAcceptsInput(target.list))
		return false;
	RE::GFxValue index, selected;
	index.SetNumber(target.index);
	// onItemRollOver preserves SkyUI's disabled/animation checks, highlight
	// events, and ConfigPanel's focus ownership. Do not write _selectedIndex.
	return target.list.Invoke("onItemRollOver", nullptr, &index, 1) &&
	    target.list.GetMember("selectedIndex", &selected) && selected.IsNumber() &&
	    selected.GetNumber() == target.index;
}

bool ActivateMCMOption(RE::GFxMovieView& movie, float x, float y)
{
	MCMLaserTarget target;
	if (!ResolveMCMOptionTarget(movie, x, y, target) || !HoverMCMOption(target))
		return false;
	// Selection can execute mod callbacks. Revalidate before firing one press.
	MCMLaserTarget current;
	if (!ResolveMCMOptionTarget(movie, x, y, current) || current.index != target.index ||
	    !(current.list == target.list) || !(current.clip == target.clip))
		return false;
	std::array<RE::GFxValue, 2> args;
	args[0].SetNumber(target.index);
	args[1].SetNumber(0); // BasicList.SELECT_MOUSE
	return target.list.Invoke("onItemPress", nullptr, args.data(), 2);
}

bool RestoreMCMControllerFocus(RE::GFxMovieView& movie)
{
	RE::GFxValue panel, focus;
	if (!GetReadyMCMPanel(movie, panel) || !panel.GetMember("_focus", &focus) ||
	    !focus.IsNumber() || (focus.GetNumber() != 0 && focus.GetNumber() != 1))
		return false;
	// One handoff, not a per-frame override. Stop a parked mouse from selecting
	// a row again when a list redraws after native stick navigation.
	movie.NotifyMouseState(-10000.0f, -10000.0f, 0u, 0);
	RE::GFxValue mouseDriven;
	mouseDriven.SetBoolean(false);
	for (const char* name : { "_modList", "_subList", "_optionsList" }) {
		RE::GFxValue list;
		if (panel.GetMember(name, &list) && MCMListAcceptsInput(list))
			list.SetMember("isMouseDrivenNav", mouseDriven);
	}
	// changeFocus selects the mod-list panel or the options list appropriately.
	// Never force-enable lists: a modal dialog or pending Papyrus operation owns them.
	return panel.Invoke("changeFocus", nullptr, &focus, 1);
}
