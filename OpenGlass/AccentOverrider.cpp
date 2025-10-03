#include "pch.h"
#include "AccentOverrider.hpp"
#include "GlassKernel.hpp"
#include "uDWMProjection.hpp"
#include "Shared.hpp"
#include "GlassReflectionBrush.hpp"


using namespace OpenGlass;
namespace OpenGlass::AccentOverrider
{
	HRESULT STDMETHODCALLTYPE MyCAccent_UpdateAccentPolicy(
		uDWM::CAccent* This,
		LPCRECT rect,
		uDWM::ACCENT_POLICY* policy,
		uDWM::CBaseGeometryProxy* geometry
	);
	HRESULT STDMETHODCALLTYPE MyCAccent__UpdateSolidFill(
		uDWM::CAccent* This,
		uDWM::CRenderDataVisual* visual,
		UINT color,
		const D2D1_RECT_F& rect,
		float opacity
	);
	bool STDMETHODCALLTYPE MyCAccentBlurBehind_IsBlurBehindDirty(
		uDWM::CAccentBlurBehind* This,
		const uDWM::CWindowData* data,
		LPCRECT rectangle,
		ULONG_PTR desktopId,
		HWND hwnd
	);
	decltype(&MyCAccent_UpdateAccentPolicy) g_CAccent_UpdateAccentPolicy_Org{ nullptr };
	decltype(&MyCAccent__UpdateSolidFill) g_CAccent__UpdateSolidFill_Org{ nullptr };
	decltype(&MyCAccentBlurBehind_IsBlurBehindDirty) g_CAccentBlurBehind_IsBlurBehindDirty_Org{ nullptr };

	bool g_disableGlassHooks{ false };
	DWORD g_taskbarCompMode{ 0 };
}

HRESULT STDMETHODCALLTYPE AccentOverrider::MyCAccent_UpdateAccentPolicy(
	uDWM::CAccent* This,
	LPCRECT rect,
	uDWM::ACCENT_POLICY* policy,
	uDWM::CBaseGeometryProxy* geometry
)
{
	if (
		policy->AccentState != 1 &&
		policy->AccentState != 3 &&
		policy->AccentState != 4
		)
	{
		return g_CAccent_UpdateAccentPolicy_Org(This, rect, policy, geometry);
	}

	HRESULT hr{ S_OK };

	const auto window = uDWM::TryGetWindowFromVisual(This);
	bool isTaskbar = false;
	if (window) {
		const HWND hwnd = window->GetData()->GetHwnd();
		if (hwnd) {
			wchar_t className[32];
			if (GetClassNameW(hwnd, className, 32)) {
				if (_wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
					isTaskbar = true;
				}
			}
		}
	}

	if (Shared::g_overrideAccent || (g_taskbarCompMode == 1 && isTaskbar))
	{
		auto accentPolicy = *policy;
		accentPolicy.AccentState = 1; // ACCENT_DISABLED
		accentPolicy.dwGradientColor = 0;
		hr = g_CAccent_UpdateAccentPolicy_Org(This, rect, &accentPolicy, geometry);
	}
	else
	{
		hr = g_CAccent_UpdateAccentPolicy_Org(This, rect, policy, geometry);
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE AccentOverrider::MyCAccent__UpdateSolidFill(
	uDWM::CAccent* This,
	uDWM::CRenderDataVisual* visual,
	UINT color,
	const D2D1_RECT_F& rect,
	float opacity
)
{
	// NEW STRUCTURE: Check for the taskbar override first.
	if (g_taskbarCompMode == 1)
	{
		// This block is now self-contained and runs regardless of GlassOverrideAccent.
		const auto window = uDWM::TryGetWindowFromVisual(This);
		bool isTaskbar = false;
		if (window) {
			const HWND hwnd = window->GetData()->GetHwnd();
			if (hwnd) {
				wchar_t className[32];
				if (GetClassNameW(hwnd, className, 32)) {
					if (_wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
						isTaskbar = true;
					}
				}
			}
		}

		const bool hasMaximizedWindows = uDWM::CDesktopManager::GetInstance()->HasMaximizedWindows();
		D2D1_COLOR_F finalColor;
		bool reflectionEnabled = true;

		if (isTaskbar && hasMaximizedWindows)
		{
			// Opaque taskbar logic...
			const auto accentColor = GlassKernel::GetBlendingSourceColor(true);
			if (accentColor.r == 0.0f && accentColor.g == 0.0f && accentColor.b == 0.0f) {
				finalColor = { 0.0f, 0.0f, 0.0f, 1.0f };
			}
			else {
				const float saturation = std::max(accentColor.r, std::max(accentColor.g, accentColor.b)) - std::min(accentColor.r, std::min(accentColor.g, accentColor.b));
				const float luminance = (0.2126f * accentColor.r) + (0.7152f * accentColor.g) + (0.0722f * accentColor.b);
				if (luminance > 0.8f && saturation < 0.1f) {
					finalColor = { 0.12f, 0.13f, 0.14f, 1.0f };
				}
				else {
					finalColor.r = std::max(0.0f, accentColor.r * 0.16f - 0.013f);
					finalColor.g = std::max(0.0f, accentColor.g * 0.056f + 0.0122f);
					finalColor.b = std::max(0.0f, accentColor.b * 0.054f + 0.0158f);
				}
			}
			finalColor.a = 1.0f;
			reflectionEnabled = false;
		}
		else
		{
			// Standard glass logic for all other windows when taskbar mode is on.
			const auto opaque = Shared::IsTransparencyDisabled();
			finalColor = GlassKernel::RealizeWindowColorization(
				GlassKernel::GetBlendingBaseColor(opaque, false),
				GlassKernel::GetBlendingSourceColor(true),
				GlassKernel::GetColorizationBlendingOpacity(true, false),
				opaque,
				false
			).effectiveBlendColor;
			finalColor.a = GlassKernel::AlphaChannelReinterpreter(true, false).ToFloat();
		}

		// Shared rendering logic...
		RETURN_IF_FAILED(visual->ClearInstructions());
		winrt::com_ptr<uDWM::CRgnGeometryProxy> geometry{ nullptr };
		RETURN_IF_FAILED(uDWM::ResourceHelper::CreateGeometryFromHRGN(
			wil::unique_hrgn{ CreateRectRgn(static_cast<LONG>(rect.left), static_cast<LONG>(rect.top), static_cast<LONG>(rect.right), static_cast<LONG>(rect.bottom)) }.get(),
			geometry.put()
		));
		winrt::com_ptr<uDWM::CSolidColorLegacyMilBrushProxy> colorBrush{ nullptr };
		RETURN_IF_FAILED(uDWM::CDesktopManager::GetInstance()->GetCompositor()->CreateSolidColorLegacyMilBrushProxy(colorBrush.put()));
		RETURN_IF_FAILED(colorBrush->Update(1.0, finalColor));
		winrt::com_ptr<uDWM::CDrawGeometryInstruction> colorInstruction{ nullptr };
		RETURN_IF_FAILED(uDWM::CDrawGeometryInstruction::Create(colorBrush.get(), geometry.get(), colorInstruction.put()));
		RETURN_IF_FAILED(visual->AddInstruction(colorInstruction.get()));
		if (window && reflectionEnabled) {
			if (const auto reflectionBrush = GlassReflectionBrush::GetOrCreate(window, true)) {
				RETURN_IF_FAILED(reflectionBrush->Update(
					(Shared::g_reflectionPolicy & Shared::ReflectionPolicy::NonClient) ? Shared::g_reflectionIntensity : 0.f,
					GlassReflectionBrush::CalculateTargetViewport(visual->GetLocalToParentVisualOffset(window->GetTransformParent()), Shared::g_reflectionParallaxIntensity, window->IsRTLMirrored(), visual->GetWidth(), visual->GetScale()),
					D2D1::RectF(), nullptr, DWM::MilBrushMappingMode::Absolute, DWM::MilBrushMappingMode::Absolute, nullptr, nullptr, DWM::MilStretch::None, DWM::MilTileMode::Extend, DWM::MilHorizontalAlignment::Left, DWM::MilVerticalAlignment::Top, nullptr
				));
				winrt::com_ptr<uDWM::CDrawGeometryInstruction> reflectionInstruction{ nullptr };
				RETURN_IF_FAILED(uDWM::CDrawGeometryInstruction::Create(reflectionBrush.get(), geometry.get(), reflectionInstruction.put()));
				RETURN_IF_FAILED(visual->AddInstruction(reflectionInstruction.get()));
			}
		}
		return S_OK;
	}
	// If taskbar mode is off, check for the general glass override.
	else if (Shared::g_overrideAccent)
	{
		// This is the original glass logic from before the taskbar edit.
		RETURN_IF_FAILED(visual->ClearInstructions());
		winrt::com_ptr<uDWM::CRgnGeometryProxy> geometry{ nullptr };
		RETURN_IF_FAILED(uDWM::ResourceHelper::CreateGeometryFromHRGN(
			wil::unique_hrgn{ CreateRectRgn(static_cast<LONG>(rect.left), static_cast<LONG>(rect.top), static_cast<LONG>(rect.right), static_cast<LONG>(rect.bottom)) }.get(),
			geometry.put()
		));

		const auto window = uDWM::TryGetWindowFromVisual(This);
		{
			winrt::com_ptr<uDWM::CSolidColorLegacyMilBrushProxy> brush{ nullptr };
			RETURN_IF_FAILED(uDWM::CDesktopManager::GetInstance()->GetCompositor()->CreateSolidColorLegacyMilBrushProxy(brush.put()));
			const auto opaque = Shared::IsTransparencyDisabled();
			auto glassColor = GlassKernel::RealizeWindowColorization(
				GlassKernel::GetBlendingBaseColor(opaque, false),
				GlassKernel::GetBlendingSourceColor(true),
				GlassKernel::GetColorizationBlendingOpacity(true, false),
				opaque,
				false
			).effectiveBlendColor;
			glassColor.a = GlassKernel::AlphaChannelReinterpreter(true, false).ToFloat();
			RETURN_IF_FAILED(brush->Update(1.0, glassColor));
			winrt::com_ptr<uDWM::CDrawGeometryInstruction> instruction{ nullptr };
			RETURN_IF_FAILED(uDWM::CDrawGeometryInstruction::Create(brush.get(), geometry.get(), instruction.put()));
			RETURN_IF_FAILED(visual->AddInstruction(instruction.get()));
		}

		if (window) {
			if (const auto brush = GlassReflectionBrush::GetOrCreate(window, true)) {
				RETURN_IF_FAILED(brush->Update(
					(Shared::g_reflectionPolicy & Shared::ReflectionPolicy::NonClient) ? Shared::g_reflectionIntensity : 0.f,
					GlassReflectionBrush::CalculateTargetViewport(visual->GetLocalToParentVisualOffset(window->GetTransformParent()), Shared::g_reflectionParallaxIntensity, window->IsRTLMirrored(), visual->GetWidth(), visual->GetScale()),
					D2D1::RectF(), nullptr, DWM::MilBrushMappingMode::Absolute, DWM::MilBrushMappingMode::Absolute, nullptr, nullptr, DWM::MilStretch::None, DWM::MilTileMode::Extend, DWM::MilHorizontalAlignment::Left, DWM::MilVerticalAlignment::Top, nullptr
				));
				winrt::com_ptr<uDWM::CDrawGeometryInstruction> instruction{ nullptr };
				RETURN_IF_FAILED(uDWM::CDrawGeometryInstruction::Create(brush.get(), geometry.get(), instruction.put()));
				RETURN_IF_FAILED(visual->AddInstruction(instruction.get()));
			}
		}
		return S_OK;
	}

	// If neither override is active, fall back to the default DWM behavior.
	return g_CAccent__UpdateSolidFill_Org(This, visual, color, rect, opacity);
}

bool STDMETHODCALLTYPE AccentOverrider::MyCAccentBlurBehind_IsBlurBehindDirty(
	[[maybe_unused]] uDWM::CAccentBlurBehind* This,
	[[maybe_unused]] const uDWM::CWindowData* data,
	[[maybe_unused]] LPCRECT rectangle,
	[[maybe_unused]] ULONG_PTR desktopId,
	[[maybe_unused]] HWND hwnd
)
{
	return false;
}

void AccentOverrider::Update(GlassEngine::UpdateType type)
{
	if (type & GlassEngine::UpdateType::Backdrop)
	{
		Shared::g_overrideAccent = static_cast<bool>(GlassEngine::GetDwordFromRegistry(L"GlassOverrideAccent"));
		g_taskbarCompMode = GlassEngine::GetDwordFromRegistry(L"TaskbarCompMode");
	}
}

void AccentOverrider::Startup()
{
	DWORD value{ 0ul };
	wil::reg::get_value_dword_nothrow(
		GlassEngine::GetDwmLocalMachineKey(),
		L"DisabledHooks",
		&value
	);
	g_disableGlassHooks = (value & 2) != 0;

	if (g_disableGlassHooks)
	{
		return;
	}
	g_taskbarCompMode = GlassEngine::GetDwordFromRegistry(L"TaskbarCompMode");
	uDWM::g_projectionArray.ApplyToVariable("CAccent::UpdateAccentPolicy", g_CAccent_UpdateAccentPolicy_Org);
	uDWM::g_projectionArray.ApplyToVariable("CAccent::_UpdateSolidFill", g_CAccent__UpdateSolidFill_Org);
	uDWM::g_projectionArray.ApplyToVariable("CAccentBlurBehind::IsBlurBehindDirty", g_CAccentBlurBehind_IsBlurBehindDirty_Org);

	THROW_IF_FAILED(
		HookHelper::Detours::Write([]() static
	{
		HookHelper::Detours::Attach(&g_CAccent_UpdateAccentPolicy_Org, MyCAccent_UpdateAccentPolicy);
		HookHelper::Detours::Attach(&g_CAccent__UpdateSolidFill_Org, MyCAccent__UpdateSolidFill);
		if (uDWM::g_versionInfo.build < os::build_w11_22h2)
		{
			HookHelper::Detours::Attach(&g_CAccentBlurBehind_IsBlurBehindDirty_Org, MyCAccentBlurBehind_IsBlurBehindDirty);
		}
	})
	);
}

void AccentOverrider::Shutdown()
{
	if (g_disableGlassHooks)
	{
		return;
	}

	THROW_IF_FAILED(
		HookHelper::Detours::Write([]() static
	{
		HookHelper::Detours::Detach(&g_CAccent_UpdateAccentPolicy_Org, MyCAccent_UpdateAccentPolicy);
		HookHelper::Detours::Detach(&g_CAccent__UpdateSolidFill_Org, MyCAccent__UpdateSolidFill);
		if (uDWM::g_versionInfo.build < os::build_w11_22h2)
		{
			HookHelper::Detours::Detach(&g_CAccentBlurBehind_IsBlurBehindDirty_Org, MyCAccentBlurBehind_IsBlurBehindDirty);
		}
	})
	);
}
