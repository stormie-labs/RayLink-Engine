#include "StdAfx.h"

#include "UIMMShniaga.h"
#include "UICursor.h"
#include "UIStatic.h"
#include "UIScrollView.h"
#include "UIXmlInit.h"
#include "MMsound.h"
#include "game_base_space.h"
#include "../level.h"
#include "object_broker.h"
#include <math.h>
#include "../Actor.h"
#include "../saved_game_wrapper.h"
#include "../login_manager.h"
#include "MainMenu.h"
#include "../coop/coop_manager.h" // Anomaly Coop

extern string_path g_last_saved_game;

// Anomaly Coop (F3): in a co-op session nobody saves or loads from the menu, the host included (the host restores
// from the emergency autosaves in the co-op window). The buttons of the save/load entries are not created.
// The names are the same in stock Anomaly and in GAMMA (ui_mm_main_16.xml, MCM copy).
static bool coop_hidden_menu_button(LPCSTR name)
{
	if (!name || !coop::mgr().active())
		return false;
	return !xr_strcmp(name, "btn_save") || !xr_strcmp(name, "btn_load") || !xr_strcmp(name, "btn_lastsave");
}

CUIMMShniaga::CUIMMShniaga()
{
	m_sound = xr_new<CMMSound>();

	m_view = xr_new<CUIScrollView>();
	AttachChild(m_view);
	m_shniaga = xr_new<CUIStatic>();
	AttachChild(m_shniaga);
	m_magnifier = xr_new<CUIStatic>();
	m_shniaga->AttachChild(m_magnifier);
	m_magnifier->SetPPMode();
	m_mag_pos = 0;

	m_selected = NULL;
	m_silent = false;
	m_magnifier_visible = true;
	m_offset = 0;

	m_start_time = 0;
	m_origin = 0;
	m_destination = 0;
	m_run_time = 0;

	m_flags.zero();

	m_selected_btn = -1;
	m_page = epi_none;
}

CUIMMShniaga::~CUIMMShniaga()
{
	xr_delete(m_magnifier);
	xr_delete(m_shniaga);
	xr_delete(m_view);
	xr_delete(m_sound);

	delete_data(m_buttons);
	delete_data(m_buttons_new);
	delete_data(m_buttons_new_network);
}

extern CActor* g_actor;

void CUIMMShniaga::InitShniaga(CUIXml& xml_doc, LPCSTR path)
{
	string256 _path;

	CUIXmlInit::InitWindow(xml_doc, path, 0, this);
	strconcat(sizeof(_path), _path, path, ":shniaga:magnifire");
	CUIXmlInit::InitStatic(xml_doc, _path, 0, m_magnifier);
	m_mag_pos = m_magnifier->GetWndPos().x;
	strconcat(sizeof(_path), _path, path, ":shniaga");
	CUIXmlInit::InitStatic(xml_doc, _path, 0, m_shniaga);
	strconcat(sizeof(_path), _path, path, ":buttons_region");
	CUIXmlInit::InitScrollView(xml_doc, _path, 0, m_view);
	strconcat(sizeof(_path), _path, path, ":shniaga:magnifire:y_offset");
	m_offset = xml_doc.ReadFlt(_path, 0, 0);

	if (!g_pGameLevel || !g_pGameLevel->bReady)
	{
		if (!*g_last_saved_game || !CSavedGameWrapper::valid_saved_game(g_last_saved_game))
			CreateList(m_buttons, xml_doc, "menu_main");
		else
			CreateList(m_buttons, xml_doc, "menu_main_last_save");

		CreateList(m_buttons_new, xml_doc, "menu_new_game");
	}
	else
	{
		if (GameID() == eGameIDSingle)
		{
			VERIFY(Actor());
			if (g_actor && !Actor()->g_Alive())
				CreateList(m_buttons, xml_doc, "menu_main_single_dead");
			else
				CreateList(m_buttons, xml_doc, "menu_main_single");
		}
		else
			CreateList(m_buttons, xml_doc, "menu_main_mm");
	}
	CreateList(m_buttons_new_network, xml_doc, "menu_network_game");

	ShowMain();

	m_sound->Init(xml_doc, "menu_sound");
	if (!g_pGameLevel || !g_pGameLevel->bReady)
	{
		m_sound->music_Play();
	}
}

void CUIMMShniaga::OnDeviceReset()
{
}

extern CActor* g_actor;

void CUIMMShniaga::CreateList(xr_vector<CUITextWnd*>& lst, CUIXml& xml_doc, LPCSTR path)
{
	CGameFont* pF;
	u32 color;
	float button_height = xml_doc.ReadAttribFlt("button", 0, "h");
	R_ASSERT(button_height);

	CUIXmlInit::InitFont(xml_doc, path, 0, color, pF);
	R_ASSERT(pF);

	int nodes_num = xml_doc.GetNodesNum(path, 0, "btn");

	XML_NODE* tab_node = xml_doc.NavigateToNode(path, 0);
	xml_doc.SetLocalRoot(tab_node);

	CUITextWnd* st;

	for (int i = 0; i < nodes_num; ++i)
	{
		if (coop_hidden_menu_button(xml_doc.ReadAttrib("btn", i, "name"))) // Anomaly Coop
			continue;
		st = xr_new<CUITextWnd>();
		st->SetWndPos(Fvector2().set(0, 0));
		st->SetWndSize(Fvector2().set(m_view->GetDesiredChildWidth(), button_height));
		st->SetFont(pF);
		st->SetTextComplexMode(false);
		st->SetTextST(xml_doc.ReadAttrib("btn", i, "caption"));

		//		float font_height			= st->GetFont()->GetHeight();
		//		UI().ClientToScreenScaledHeight(font_height);

		//.		st->SetTextOffset			(0, (button_height-font_height)/2.0f);
		st->SetTextColor(color);
		st->SetTextAlignment(CGameFont::alCenter);
		st->SetVTextAlignment(valCenter);
		st->SetWindowName(xml_doc.ReadAttrib("btn", i, "name"));
		st->SetMessageTarget(this);


		lst.push_back(st);
	}
	xml_doc.SetLocalRoot(xml_doc.GetRoot());
}

void CUIMMShniaga::SetPage(enum_page_id page_id, LPCSTR xml_file, LPCSTR xml_path)
{
	VERIFY(m_page != page_id);
	xr_vector<CUITextWnd*>* lst = NULL;
	switch (page_id)
	{
	case epi_main:
		{
			lst = &m_buttons;
		}
		break;
	case epi_new_game:
		{
			lst = &m_buttons_new;
		}
		break;
	case epi_new_network_game:
		{
			lst = &m_buttons_new_network;
		}
		break;
	}; //switch (page_id)
	delete_data(*lst);

	CUIXml tmp_xml;
	tmp_xml.Load(CONFIG_PATH, UI_PATH, xml_file);
	CreateList(*lst, tmp_xml, xml_path);
}

void CUIMMShniaga::ShowPage(enum_page_id page_id)
{
	switch (page_id)
	{
	case epi_main:
		{
			ShowMain();
		}
		break;
	case epi_new_game:
		{
			ShowNewGame();
		}
		break;
	case epi_new_network_game:
		{
			ShowNetworkGame();
		}
		break;
	}; //switch (page_id)
}


xr_vector<CUITextWnd*>* CUIMMShniaga::PageList(enum_page_id page)
{
	switch (page)
	{
	case epi_main: return &m_buttons;
	case epi_new_game: return &m_buttons_new;
	case epi_new_network_game: return &m_buttons_new_network;
	default: return nullptr;
	}
}

// A page whose list came out empty (every entry of the XML list hidden) shows nothing and selects nothing, instead of
// reading the first element of an empty vector.
void CUIMMShniaga::ShowMain()
{
	m_page = epi_main;
	m_view->Clear();
	for (u32 i = 0; i < m_buttons.size(); i++)
		m_view->AddWindow(m_buttons[i], false);

	if (m_buttons.empty())
		m_selected = NULL;
	else
		SelectBtn(m_buttons[0]);
}

void CUIMMShniaga::ShowNewGame()
{
	m_page = epi_new_game;
	m_view->Clear();
	for (u32 i = 0; i < m_buttons_new.size(); i++)
		m_view->AddWindow(m_buttons_new[i], false);

	if (m_buttons_new.empty())
		m_selected = NULL;
	else
		SelectBtn(m_buttons_new[0]);
}

void CUIMMShniaga::ShowNetworkGame()
{
	m_page = epi_new_network_game;
	m_view->Clear();

	for (u32 i = 0,
	         count = m_buttons_new_network.size(); i < count; ++i)
	{
		m_view->AddWindow(m_buttons_new_network[i], false);
	}
	if (m_buttons_new_network.empty())
		m_selected = NULL;
	else
		SelectBtn(m_buttons_new_network[0]);
}


bool CUIMMShniaga::IsButton(CUIWindow* st)
{
	for (u32 i = 0; i < m_buttons.size(); ++i)
		if (m_buttons[i] == st)
			return true;

	for (u32 i = 0; i < m_buttons_new.size(); ++i)
		if (m_buttons_new[i] == st)
			return true;

	for (u32 i = 0, count = m_buttons_new_network.size(); i < count; ++i)
		if (m_buttons_new_network[i] == st)
			return true;

	return false;
}

void CUIMMShniaga::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	CUIWindow::SendMessage(pWnd, msg, pData);
	if (IsButton(pWnd))
	{
		switch (msg)
		{
		case WINDOW_FOCUS_RECEIVED:
			SelectBtn(pWnd);
			break;
		}
	}
}

void CUIMMShniaga::SelectBtn(int btn)
{
	m_view->Update();

	R_ASSERT(btn >= 0);
	xr_vector<CUITextWnd*>* lst = PageList(m_page);
	if (!lst || btn >= int(lst->size()))
		return;
	m_selected = (*lst)[btn];

	m_selected_btn = btn;
	ProcessEvent(E_Begin);
}

void CUIMMShniaga::SelectBtn(CUIWindow* btn)
{
	R_ASSERT(m_page >= 0);
	// the list of the page that is shown, up to its own size (the old loop ran to the size of the main list for every page)
	xr_vector<CUITextWnd*>* lst = PageList(m_page);
	if (!lst)
		return;
	for (int i = 0; i < (int)lst->size(); ++i)
	{
		if ((*lst)[i] == btn)
		{
			SelectBtn(i);
			return;
		}
	}
}

void CUIMMShniaga::Draw()
{
	if (m_silent)
		return;
	CUIWindow::Draw();
}

void CUIMMShniaga::Update()
{
	if (m_start_time > Device.dwTimeContinual - m_run_time)
	{
		Fvector2 pos = m_shniaga->GetWndPos();
		pos.y = this->pos(m_origin, m_destination, Device.dwTimeContinual - m_start_time);
		m_shniaga->SetWndPos(pos);
	}
	else
		ProcessEvent(E_Stop);


	ProcessEvent(E_Update); // the menu music is kept going from here

	if (m_silent)
		return; // nothing under the cursor reacts: no focus, no wheel
	CUIWindow::Update();
}


bool CUIMMShniaga::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
	if (m_silent)
		return false;
	Fvector2 pos = UI().GetUICursor().GetCursorPosition();
	Frect r;
	m_magnifier->GetAbsoluteRect(r);
	if (WINDOW_LBUTTON_DOWN == mouse_action && r.in(pos.x, pos.y))
	{
		OnBtnClick();
	}

	return CUIWindow::OnMouseAction(x, y, mouse_action);
}

void CUIMMShniaga::OnBtnClick()
{
	if (!m_selected)
		return;
	if (0 == xr_strcmp("btn_new_game", m_selected->WindowName()))
		ShowNewGame();
	else if (0 == xr_strcmp("btn_new_back", m_selected->WindowName()))
		ShowMain();
	else
	{
		if (CUIWindow* target = GetMessageTarget())
			target->SendMessage(m_selected, BUTTON_CLICKED);
		ProcessEvent(E_Finilize);
	}
}

int CUIMMShniaga::GetButtonCount()
{
	xr_vector<CUITextWnd*>* lst = PageList(m_page);
	return lst ? int(lst->size()) : 0;
}

LPCSTR CUIMMShniaga::GetButtonName(int i)
{
	xr_vector<CUITextWnd*>* lst = PageList(m_page);
	if (!lst || i < 0 || i >= int(lst->size()))
		return nullptr;
	return (*lst)[i]->WindowName().c_str();
}

LPCSTR CUIMMShniaga::GetButtonCaption(int i)
{
	xr_vector<CUITextWnd*>* lst = PageList(m_page);
	if (!lst || i < 0 || i >= int(lst->size()))
		return nullptr;
	return (*lst)[i]->GetText();
}

bool CUIMMShniaga::PressButton(LPCSTR name)
{
	if (!name || !name[0])
		return false;
	xr_vector<CUITextWnd*>* lst = PageList(m_page);
	if (!lst)
		return false;
	for (u32 i = 0; i < lst->size(); ++i)
	{
		if (0 == xr_strcmp(name, (*lst)[i]->WindowName().c_str()))
		{
			// what a click does: the button is the selected one, then the same OnBtnClick (btn_new_game and btn_new_back
			// change the page, any other name goes to the message target as BUTTON_CLICKED, where the callbacks by name are)
			m_selected = (*lst)[i];
			m_selected_btn = int(i);
			OnBtnClick();
			return true;
		}
	}
	return false;
}

#include <dinput.h>

bool CUIMMShniaga::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
	if (m_silent)
		return false;
	if (WINDOW_KEY_PRESSED == keyboard_action)
	{
		switch (dik)
		{
		case DIK_UP:
			if (m_selected_btn > 0)
				SelectBtn(m_selected_btn - 1);
			return true;
		case DIK_DOWN:
			if (m_selected_btn < BtnCount() - 1)
				SelectBtn(m_selected_btn + 1);
			return true;
		case DIK_RETURN:
			OnBtnClick();
			return true;
		case DIK_ESCAPE:
			if (m_page != epi_main)
				ShowMain();
			return true;
		}
	}


	return CUIWindow::OnKeyboardAction(dik, keyboard_action);
}

int CUIMMShniaga::BtnCount()
{
	R_ASSERT(-1);
	xr_vector<CUITextWnd*>* lst = PageList(m_page);
	return lst ? (int)lst->size() : -1;
}

float CUIMMShniaga::pos(float x1, float x2, u32 t)
{
	float x = 0;

	if (t >= 0 && t <= m_run_time)
		x = log(1 + (t * 10.0f) / m_run_time) / log(11.0f);
	else if (t <= 0)
		x = 0;
	else if (t > m_run_time)
		x = 1;

	x *= abs(x2 - x1);

	if (x2 - x1 < 0)
		return x1 - x;
	else
		return x1 + x;
}

bool b_shniaganeed_pp = true;

// The magnifier is drawn by the post-process pass (RegisterPPDraw), by its position and not by its visibility: it is moved off the
// screen when it must not show.
void CUIMMShniaga::ApplyMagnifier()
{
	Fvector2 pos = m_magnifier->GetWndPos();
	if (m_magnifier_visible && !m_silent)
		pos.x = m_mag_pos;
	else
		pos.x = 1025;
	m_magnifier->SetWndPos(pos);
}

void CUIMMShniaga::SetVisibleMagnifier(bool f)
{
	b_shniaganeed_pp = f;
	m_magnifier_visible = f;
	ApplyMagnifier();
}

// Show(false) alone would not do: a hidden window is not updated by its parent (the menu music, which Update keeps looping,
// would stop after the first track), the magnifier is drawn by the post-process pass whatever the visibility of the window,
// and the message routing of a click does not depend on it at all.
void CUIMMShniaga::SetSilent(bool on)
{
	if (on == m_silent)
		return;
	m_silent = on;
	ApplyMagnifier();
}

void CUIMMShniaga::ProcessEvent(EVENT ev)
{
	switch (ev)
	{
	case E_Begin:
		{
			// init whell sound
			if (!m_silent)
				m_sound->whell_Play();

			// calculate moving params
			if (!m_selected)
				break;
			m_start_time = Device.dwTimeContinual;
			m_origin = m_shniaga->GetWndPos().y;
			m_destination = m_selected->GetWndPos().y - m_magnifier->GetWndPos().y;
			m_destination += m_offset;
			m_run_time = u32((log(1 + abs(m_origin - m_destination)) / log(GetHeight())) * 300);
			if (m_run_time < 100)
				m_run_time = 100;

			// reset flags
			m_flags.set(fl_SoundFinalized, FALSE);
			m_flags.set(fl_MovingStoped, FALSE);
		}
		break;
	case E_Finilize:
		if (!m_flags.test(fl_SoundFinalized))
		{
			if (!m_silent)
				m_sound->whell_Click();

			m_flags.set(fl_SoundFinalized, TRUE);
		}
		break;
	case E_Stop:
		if (!m_flags.test(fl_MovingStoped))
		{
			m_sound->whell_Stop();


			Fvector2 pos = m_shniaga->GetWndPos();
			pos.y = m_destination;
			m_shniaga->SetWndPos(pos);

			m_flags.set(fl_MovingStoped, TRUE);
		}
		break;
	case E_Update:
		if (!g_pGameLevel || !g_pGameLevel->bReady)
		{
			m_sound->music_Update();
		}
		break;
	}
}
