#pragma once

#include "UIWindow.h"

class CUIStatic;
class CUITextWnd;
class CUIXml;
class CUIScrollView;
class CMMSound;

class CUIMMShniaga : public CUIWindow,
                     public CDeviceResetNotifier
{
public:

	CUIMMShniaga();
	virtual ~CUIMMShniaga();

	void InitShniaga(CUIXml& xml_doc, LPCSTR path);
	virtual void Update();
	virtual void Draw();

	virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
	virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action);
	virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = 0);
	void SetVisibleMagnifier(bool f);
	virtual void OnDeviceReset();

	virtual CUIWindow* ui_cast_window() { return this; }

	enum enum_page_id
	{
		epi_main = 0x00,
		epi_new_game,
		epi_new_network_game,
		epi_none
	}; //enum	enum_page_id
	void SetPage(enum_page_id page_id, LPCSTR xml_file, LPCSTR xml_path);
	void ShowPage(enum_page_id page_id);

	// Script access to what the widget shows, so a script can draw its own look while the widget stays the source of truth for
	// what the buttons are in the state the menu was opened in (the XML lists, with the buttons mods add there). The list is
	// built when the widget is created, so it is valid as soon as it exists. Indexes are 0-based.
	int GetPage() const { return int(m_page); } // enum_page_id of the list shown: 0 main, 1 new game, 2 network game
	int GetButtonCount(); // buttons of the page that is shown
	LPCSTR GetButtonName(int i); // the name a click is reported under (the callbacks of the script window are named by it); nil out of range
	LPCSTR GetButtonCaption(int i); // the translated caption; nil out of range
	bool PressButton(LPCSTR name); // what a click on that button of the shown page does (same SendMessage route); false when there is none
	// Silent: the widget stays alive (its music keeps playing and looping, its lists keep being valid and PressButton works) but
	// draws nothing, takes no mouse or key, does not react to the cursor and plays no wheel or click sound.
	void SetSilent(bool on);
	bool IsSilent() const { return m_silent; }
protected:
	typedef enum
	{
		E_Begin = 0,
		E_Update,
		E_Finilize,
		E_Stop
	} EVENT;

	void SelectBtn(int btn);
	void SelectBtn(CUIWindow* btn);
	int BtnCount();
	void OnBtnClick();
	xr_vector<CUITextWnd*>* PageList(enum_page_id page); // nullptr for epi_none
	void ApplyMagnifier();

	void ProcessEvent(EVENT ev);

	bool IsButton(CUIWindow* st);
	void CreateList(xr_vector<CUITextWnd*>& lst, CUIXml& xml_doc, LPCSTR path);
	void ShowMain();
	void ShowNewGame();
	void ShowNetworkGame();
	float pos(float x1, float x2, u32 t);

	CUIStatic* m_shniaga;
	CUIStatic* m_magnifier;
	CUIScrollView* m_view;

	u32 m_start_time;
	u32 m_run_time;
	float m_origin;
	float m_destination;
	float m_mag_pos;
	float m_offset;

	xr_vector<CUITextWnd*> m_buttons;
	xr_vector<CUITextWnd*> m_buttons_new;
	xr_vector<CUITextWnd*> m_buttons_new_network;

	bool m_silent;
	bool m_magnifier_visible;

	int m_selected_btn;
	enum_page_id m_page;
	CUIWindow* m_selected;
	CMMSound* m_sound;

	//	Fvector2				m_wheel_size[2];
	enum { fl_SoundFinalized = 1, fl_MovingStoped = 2 };

	Flags32 m_flags;
};
