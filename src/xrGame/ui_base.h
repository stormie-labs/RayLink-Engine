#pragma once

class CUICursor;
class CUIGameCustom;

#include "ui_defs.h"


class CDeviceResetNotifier : public pureDeviceReset
{
public:
	CDeviceResetNotifier() { Device.seqDeviceReset.Add(this,REG_PRIORITY_NORMAL); };
	virtual ~CDeviceResetNotifier() { Device.seqDeviceReset.Remove(this); };

	virtual void OnDeviceReset()
	{
	};
};

struct CFontManager : public pureDeviceReset
{
	CFontManager();
	~CFontManager();

	typedef xr_vector<CGameFont**> FONTS_VEC;
	typedef FONTS_VEC::iterator FONTS_VEC_IT;
	FONTS_VEC m_all_fonts;
	void Render();

	// hud font
	CGameFont* pFontMedium;
	CGameFont* pFontDI;

	CGameFont* pFontArial14;
	CGameFont* pFontGraffiti19Russian;
	CGameFont* pFontGraffiti22Russian;
	CGameFont* pFontLetterica16Russian;
	CGameFont* pFontLetterica18Russian;
	CGameFont* pFontGraffiti32Russian;
	CGameFont* pFontGraffiti50Russian;
	CGameFont* pFontLetterica25;
	CGameFont* pFontStat;

	void InitializeFonts();
	void InitializeFont(CGameFont*& F, LPCSTR section, u32 flags = 0);
	LPCSTR GetFontTexName(LPCSTR section);

	// Fonts a script adds at run time (RegisterFont). Each lives in an entry that never moves, because m_all_fonts keeps the
	// address of its `font`: rendering, the device reset (a new height can mean another atlas) and the destruction go through
	// that list like for the stock fonts. A name stays in the list even if its atlas was missing (font == nullptr): XML that
	// names it then gets a stock font instead of an assert.
	struct SExtraFont
	{
		shared_str name;
		shared_str texture; // the family given to RegisterFont (see ResolveFontFamily)
		shared_str shader;
		float target_height; // px the tier is picked for (0 = the screen height, followed through device resets)
		CGameFont* font;
		bool fallback_logged;
	};
	xr_vector<SExtraFont*> m_extra_fonts;

	// The font an XML `font="name"` / Lua GetFontByName(name) means: the stock names (graffiti19, letterica18, medium, small,
	// di ...) first, then the registered ones. nullptr if there is none.
	CGameFont* FindByName(LPCSTR name);
	// Adds (or, for a name already registered, updates in place) a font whose atlas family `texture_base` is found by
	// ResolveFontFamily. Idempotent; false (and a log line, never an assert) for a name that is not usable, a name of a stock
	// font, a missing or broken atlas. `shader` null or empty = "font". `target_height` > 0 (px, may be fractional) picks the
	// atlas tier for that height instead of the screen's (a UI laid out on a part of the screen); calling again with another
	// value switches the tier in place. 0 (the default) = the screen height, followed when the device is reset.
	bool RegisterFont(LPCSTR name, LPCSTR texture_base, LPCSTR shader, float target_height = 0.f);
	// For XML: a name that was given to RegisterFont but has no font (its atlas is missing) -> the stock font to use instead
	// (logged once per name); nullptr for any other name.
	CGameFont* ExtraFallback(LPCSTR name);

	virtual void OnDeviceReset();

private:
	SExtraFont* FindExtra(LPCSTR name);
	void InitializeExtraFonts();
};


class ui_core : public CDeviceResetNotifier
{
	C2DFrustum m_2DFrustum;
	C2DFrustum m_2DFrustumPP;
	C2DFrustum m_FrustumLIT;

	bool m_bPostprocess;

	CFontManager* m_pFontManager;
	CUICursor* m_pUICursor;

	Fvector2 m_pp_scale_;
	Fvector2 m_scale_;
	Fvector2* m_current_scale;

	// Clip-frustum stack: base is the screen frustum, tighter convex polys push on top (twin of m_Scissors).
	xr_stack<const C2DFrustum*> m_clip_stack;

public:
	xr_stack<Frect> m_Scissors;

	ui_core();
	~ui_core();
	CFontManager& Font() { return *m_pFontManager; }
	CUICursor& GetUICursor() { return *m_pUICursor; }

	IC float ClientToScreenScaledX(float left) const { return left * m_current_scale->x; };
	IC float ClientToScreenScaledY(float top) const { return top * m_current_scale->y; };
	void ClientToScreenScaled(Fvector2& dest, float left, float top) const;
	void ClientToScreenScaled(Fvector2& src_and_dest) const;
	void ClientToScreenScaledWidth(float& src_and_dest) const;
	void ClientToScreenScaledHeight(float& src_and_dest) const;
	void AlignPixel(float& src_and_dest) const;

	const C2DFrustum& ScreenFrustum() const { return (m_bPostprocess) ? m_2DFrustumPP : m_2DFrustum; }
	C2DFrustum& ScreenFrustumLIT() { return m_FrustumLIT; }
	const C2DFrustum& ActiveClipFrustum() const { return m_clip_stack.empty() ? ScreenFrustum() : *m_clip_stack.top(); }
	void PushClipFrustum(const C2DFrustum* f) { m_clip_stack.push(f); }
	void PopClipFrustum() { m_clip_stack.pop(); }
	bool HasCustomClip() const { return !m_clip_stack.empty(); }
	void PushScissor(const Frect& r, bool overlapped = false);
	void PopScissor();

	void pp_start();
	void pp_stop();
	void RenderFont();

	virtual void OnDeviceReset();
	static bool is_widescreen();
	static u8 screenmode();
	static float get_current_kx();
	shared_str get_xml_name(LPCSTR fn);

	IUIRender::ePointType m_currentPointType;
};


extern CUICursor& GetUICursor();
extern ui_core& UI();
extern CUIGameCustom* CurrentGameUI();
