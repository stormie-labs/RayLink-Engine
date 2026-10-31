#ifndef GameFontH
#define GameFontH
#pragma once

#include "MbHelpers.h"

#include "../Include/xrRender/FontRender.h"

class ENGINE_API CGameFont
{
	friend class dxFontRender;
public:
	enum EAligment
	{
		alLeft = 0,
		alRight,
		alCenter
	};

private:
	struct String
	{
		string1024 string;
		float x, y;
		float height;
		u32 c;
		EAligment align;
	};

protected:
	Fvector2 vHalfPixel;
	Ivector2 vTS;

	EAligment eCurrentAlignment;
	u32 dwCurrentColor;
	float fCurrentHeight;
	float fCurrentX, fCurrentY;
	Fvector2 vInterval;

	Fvector* TCMap;
	float fHeight;
	float fXStep;
	float fYStep;
	float fTCHeight;
	xr_vector<String> strings;

	IFontRender* pFontRender;

	u32 nNumChars;

	u32 uFlags;

public:
	enum
	{
		fsGradient = (1 << 0),
		fsDeviceIndependent = (1 << 1),
		fsValid = (1 << 2),

		fsMultibyte = (1 << 3),

		fsForceDWORD = u32(-1)
	};

protected:
	IC const Fvector& GetCharTC(u16 c) { return TCMap[c]; }

public:
	CGameFont(LPCSTR section, u32 flags = 0);
	CGameFont(LPCSTR shader, LPCSTR texture, u32 flags = 0);
	~CGameFont();

	void Initialize(LPCSTR shader, LPCSTR texture);

	IC void SetColor(u32 C) { dwCurrentColor = C; };

	void SetHeightI(float S);
	void SetHeight(float S);

	IC float GetHeight() { return fCurrentHeight; };

	IC float WidthScale() { return (fHeight > 0.f) ? (fCurrentHeight / fHeight) : 1.f; };
	IC void SetInterval(float x, float y) { vInterval.set(x, y); };
	IC void SetInterval(const Fvector2& v) { vInterval.set(v); };
	IC void SetAligment(EAligment aligment) { eCurrentAlignment = aligment; }

	float SizeOf_(LPCSTR s);
	float SizeOf_(const wide_char* wsStr);

	float SizeOf_(const char cChar); // only ANSII

	float CurrentHeight_();

	void OutSetI(float x, float y);
	void OutSet(float x, float y);

	void MasterOut(BOOL bCheckDevice, BOOL bUseCoords, BOOL bScaleCoords, BOOL bUseSkip,
	               float _x, float _y, float _skip, LPCSTR fmt, va_list p);

	u32 smart_strlen(const char* S);
	BOOL IsMultibyte() { return (uFlags & fsMultibyte); };
	u16 SplitByWidth(u16* puBuffer, u16 uBufferSize, float fTargetWidth, const char* pszText);
	u16 GetCutLengthPos(float fTargetWidth, const char* pszText);

	void OutI(float _x, float _y, LPCSTR fmt, ...);
	void Out(float _x, float _y, LPCSTR fmt, ...);
	void OutNext(LPCSTR fmt, ...);

	void OutSkip(float val = 1.f);

	void OnRender();

	IC void Clear() { strings.clear(); };

#ifdef DEBUG
    shared_str m_font_name;
#endif
};

// Resolves which atlas key of `section` to load for the current Device.dwHeight.
ENGINE_API LPCSTR GetFontTextureName(LPCSTR section);

// The atlas of a font family that is not described by a config section (a font a script registers at run time).
// `base` is a path under $game_textures$ without extension, e.g. "ui\\my_font". The family is the files
//   <base>_h<N>.ini + .dds   an atlas authored for a screen N px high (the same idea as the texture_h<N> keys of a section)
//   <base>.ini + .dds        one atlas for every height (used only when no <base>_h<N> is usable)
// The one with the N nearest to Device.dwHeight on the log scale wins (a tie goes to the larger N), among those whose .ini
// is a complete atlas description and whose .dds exists. The language suffix of the string table (font_prefix) is
// honoured exactly as for the stock fonts. Never asserts: on false `why` says what is wrong and the log has the details;
// on true `out_texture` is the family member to hand to CGameFont::Initialize (the language suffix is added there).
// `target_height` > 0 (px, may be fractional) is the height to pick the tier for instead of Device.dwHeight: text that is
// laid out for a smaller or larger area than the screen (a UI scaled to a part of it) asks for the tier of that area.
ENGINE_API bool ResolveFontFamily(LPCSTR base, string_path& out_texture, string256& why, float target_height = 0.f);

#endif // _XR_GAMEFONT_H_
