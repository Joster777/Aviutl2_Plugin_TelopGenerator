//----------------------------------------------------------------------------------
//	テロップ自動生成 汎用プラグイン (.aux2) for AviUtl ExEdit2
//	音声認識(whisper.cpp)をプラグインに内蔵し、字幕を一覧表示してテロップとして配置します
//	修正した内容は辞書として学習し、次回以降の認識結果に自動で反映します
//----------------------------------------------------------------------------------
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <utility>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"
#include "whisper.h"
#include "ggml-backend.h"
#include "voice_separator.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#endif

//---------------------------------------------------------------------
//	定数
//---------------------------------------------------------------------
#define TELOP_WINDOW_NAME	L"テロップ自動生成"
#define TELOP_CLASS_NAME	L"TelopGeneratorWindow"

// 学習データを保存するフォルダ名 (アプリケーションデータフォルダ\data\ 配下)
#define TELOP_DATA_FOLDER	L"TelopGenerator"

#define IDC_LIST		2001
#define IDC_RUN			2002
#define IDC_CANCEL		2003
#define IDC_PLACE		2004
#define IDC_LEARN		2005
#define IDC_FOLDER		2006
#define IDC_STATUS		2007
#define IDC_SEPARATE	2008
#define IDC_ALIAS		2009
#define IDC_ALIAS_LABEL	2017
#define IDC_ALIAS_EDIT	2018
#define IDC_ALIAS_LIST	3001
#define IDC_AE_LIST		3101
#define IDC_AE_VALUE	3102
#define IDC_AE_COLOR	3103
#define IDC_AE_APPLY	3104
#define IDC_AE_REVERT	3105
#define IDC_AE_REVERT_ALL	3106
#define TELOP_ALIAS_EDIT_CLASS	L"TelopGeneratorAliasEditDialog"
#define TELOP_ALIAS_CLASS	L"TelopGeneratorAliasDialog"
#define IDC_EDIT_START	2010
#define IDC_EDIT_END	2011
#define IDC_EDIT_TEXT	2012
#define IDC_APPLY		2013
#define IDC_SPLIT		2014
#define IDC_MERGE		2015
#define IDC_DELETE		2016

#define WM_APP_PROGRESS	(WM_APP + 1)
#define WM_APP_DONE		(WM_APP + 2)
#define WM_APP_STATUS	(WM_APP + 3)

// 1つのテロップの最大文字数 (超える場合は句読点などで自動分割します)
static size_t MAX_CHARS = 24;

//---------------------------------------------------------------------
//	グローバル変数
//---------------------------------------------------------------------
static EDIT_HANDLE* g_edit = nullptr;
static LOG_HANDLE* g_logger = nullptr;
static CONFIG_HANDLE* g_config = nullptr;

static std::wstring g_dll_dir;		// プラグインDLLのあるフォルダ (models\ はこの下)
static std::wstring g_data_dir;		// 学習データのフォルダ
static std::wstring g_alias_dir;	// エイリアスのフォルダ (アプリケーションデータフォルダ\Alias)
static std::wstring g_alias_rel;	// 使用するエイリアス (Aliasフォルダからの相対パス。空なら標準のテロップ)
static HWND g_alias_label = nullptr;

static HWND g_hwnd = nullptr;
static HWND g_list = nullptr;
static HWND g_status = nullptr;
static HWND g_edit_start = nullptr;
static HWND g_edit_end = nullptr;
static HWND g_edit_text = nullptr;
static bool g_updating = false;

static std::atomic<bool> g_running(false);
static std::atomic<bool> g_cancel(false);
static HANDLE g_worker = nullptr;
static const char* volatile g_step = "";
static std::atomic<int> g_prog_base(25);

//---------------------------------------------------------------------
//	データ構造
//---------------------------------------------------------------------
struct TimeBase
{
	int rate = 30;
	int scale = 1;
};
static TimeBase g_tb;

struct Row
{
	int start = 0;			// 開始フレーム
	int end = 0;			// 終了フレーム(含まない)
	std::wstring text;		// 現在のテキスト (ユーザー修正後)
	std::wstring raw;		// 認識直後のテキスト (辞書適用後)
	bool check = true;		// 配置対象か
};
static std::vector<Row> g_rows;

struct Correction
{
	std::wstring from;
	std::wstring to;
	int count = 1;
};
static std::vector<Correction> g_corrections;

struct Placed
{
	int64_t id = 0;
	int layer = 0;
	int start = 0;
	std::wstring raw;
	std::wstring text;
};
static std::vector<Placed> g_placed;

struct Result
{
	std::vector<Row> rows;
	TimeBase tb;
	std::wstring note;	// 処理結果の補足 (音声レベルや除外した件数)
};

//---------------------------------------------------------------------
//	文字列・ファイルのユーティリティ
//---------------------------------------------------------------------
static std::string to_utf8(const std::wstring& w)
{
	if (w.empty())
	{
		return std::string();
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static std::wstring from_utf8(const std::string& s)
{
	if (s.empty())
	{
		return std::wstring();
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

static std::wstring ascii_to_w(const char* s)
{
	std::wstring w;
	while (*s)
	{
		w.push_back((wchar_t)(unsigned char)*s);
		s++;
	}
	return w;
}

static bool read_file(const std::wstring& path, std::string& out)
{
	FILE* fp = _wfopen(path.c_str(), L"rb");
	if (!fp)
	{
		return false;
	}
	out.clear();
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
	{
		out.append(buf, n);
	}
	fclose(fp);
	// UTF-8 BOM を除去
	if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
	{
		out.erase(0, 3);
	}
	return true;
}

static bool write_file(const std::wstring& path, const std::string& data)
{
	FILE* fp = _wfopen(path.c_str(), L"wb");
	if (!fp)
	{
		return false;
	}
	fwrite(data.data(), 1, data.size(), fp);
	fclose(fp);
	return true;
}

static void make_dirs(const std::wstring& path)
{
	for (size_t i = 0; i < path.size(); i++)
	{
		if (path[i] == L'\\' && i > 2)
		{
			CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
		}
	}
	CreateDirectoryW(path.c_str(), nullptr);
}

static bool file_exists(const std::wstring& path)
{
	DWORD a = GetFileAttributesW(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::vector<std::wstring> split_lines(const std::string& s)
{
	std::vector<std::wstring> lines;
	size_t pos = 0;
	while (pos <= s.size())
	{
		size_t e = s.find('\n', pos);
		if (e == std::string::npos)
		{
			e = s.size();
		}
		std::string line = s.substr(pos, e - pos);
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		if (!line.empty())
		{
			lines.push_back(from_utf8(line));
		}
		pos = e + 1;
	}
	return lines;
}

static std::vector<std::wstring> split_tab(const std::wstring& line)
{
	std::vector<std::wstring> cols;
	size_t pos = 0;
	while (true)
	{
		size_t e = line.find(L'\t', pos);
		if (e == std::wstring::npos)
		{
			cols.push_back(line.substr(pos));
			break;
		}
		cols.push_back(line.substr(pos, e - pos));
		pos = e + 1;
	}
	return cols;
}

static std::wstring single_line(std::wstring s)
{
	for (wchar_t& c : s)
	{
		if (c == L'\t' || c == L'\r' || c == L'\n')
		{
			c = L' ';
		}
	}
	return s;
}

static std::wstring trim_text(const std::wstring& s)
{
	size_t b = 0;
	size_t e = s.size();
	while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == 0x3000 || s[b] == L'\n' || s[b] == L'\r'))
	{
		b++;
	}
	while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == 0x3000 || s[e - 1] == L'\n' || s[e - 1] == L'\r'))
	{
		e--;
	}
	return s.substr(b, e - b);
}

static void log_info(const std::wstring& m)
{
	if (g_logger)
	{
		g_logger->info(g_logger, m.c_str());
	}
}

static void log_warn(const std::wstring& m)
{
	if (g_logger)
	{
		g_logger->warn(g_logger, m.c_str());
	}
}

static std::wstring get_dll_dir()
{
	HMODULE h = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&get_dll_dir, &h);
	wchar_t buf[MAX_PATH * 2] = {};
	GetModuleFileNameW(h, buf, MAX_PATH * 2);
	std::wstring p = buf;
	size_t pos = p.find_last_of(L'\\');
	if (pos != std::wstring::npos)
	{
		p.erase(pos);
	}
	return p;
}

//---------------------------------------------------------------------
//	時間の変換
//---------------------------------------------------------------------
static double frame_to_sec(int frame)
{
	return (double)frame * g_tb.scale / g_tb.rate;
}

static int sec_to_frame(double sec)
{
	return (int)(sec * g_tb.rate / g_tb.scale + 0.5);
}

static std::wstring fmt_time(int frame)
{
	double s = frame_to_sec(frame);
	int m = (int)(s / 60.0);
	double r = s - m * 60.0;
	char buf[64];
	snprintf(buf, sizeof(buf), "%d:%05.2f", m, r);
	return ascii_to_w(buf);
}

// "1:23.4" または "83.4" を秒に変換します
static bool parse_time(const std::wstring& t, double& sec)
{
	std::wstring s = trim_text(t);
	if (s.empty())
	{
		return false;
	}
	size_t c = s.find(L':');
	wchar_t* endp = nullptr;
	if (c == std::wstring::npos)
	{
		sec = wcstod(s.c_str(), &endp);
		return endp && *endp == L'\0' && sec >= 0;
	}
	double m = wcstod(s.substr(0, c).c_str(), &endp);
	if (!endp || *endp != L'\0')
	{
		return false;
	}
	std::wstring rest = s.substr(c + 1);
	double r = wcstod(rest.c_str(), &endp);
	if (!endp || *endp != L'\0')
	{
		return false;
	}
	sec = m * 60.0 + r;
	return sec >= 0;
}

//---------------------------------------------------------------------
//	学習データ (修正辞書・配置済みテロップの記録)
//---------------------------------------------------------------------
static std::wstring corrections_path()
{
	return g_data_dir + L"\\corrections.tsv";
}

static std::wstring placed_path()
{
	return g_data_dir + L"\\placed.tsv";
}

static std::wstring template_path()
{
	return g_data_dir + L"\\template.object";
}

static void load_corrections()
{
	g_corrections.clear();
	std::string data;
	if (!read_file(corrections_path(), data))
	{
		return;
	}
	for (const std::wstring& line : split_lines(data))
	{
		std::vector<std::wstring> c = split_tab(line);
		if (c.size() >= 2 && !c[0].empty())
		{
			Correction r;
			r.from = c[0];
			r.to = c[1];
			r.count = (c.size() >= 3) ? _wtoi(c[2].c_str()) : 1;
			if (r.count < 1)
			{
				r.count = 1;
			}
			g_corrections.push_back(r);
		}
	}
}

static void save_corrections()
{
	make_dirs(g_data_dir);
	std::string out;
	for (const Correction& r : g_corrections)
	{
		out += to_utf8(single_line(r.from)) + "\t" + to_utf8(single_line(r.to)) + "\t" + std::to_string(r.count) + "\n";
	}
	write_file(corrections_path(), out);
}

static void load_placed()
{
	g_placed.clear();
	std::string data;
	if (!read_file(placed_path(), data))
	{
		return;
	}
	for (const std::wstring& line : split_lines(data))
	{
		std::vector<std::wstring> c = split_tab(line);
		if (c.size() >= 5)
		{
			Placed p;
			p.id = _wtoi64(c[0].c_str());
			p.layer = _wtoi(c[1].c_str());
			p.start = _wtoi(c[2].c_str());
			p.raw = c[3];
			p.text = c[4];
			g_placed.push_back(p);
		}
	}
}

static void save_placed()
{
	make_dirs(g_data_dir);
	std::string out;
	for (const Placed& p : g_placed)
	{
		out += std::to_string(p.id) + "\t" + std::to_string(p.layer) + "\t" + std::to_string(p.start) + "\t"
			+ to_utf8(single_line(p.raw)) + "\t" + to_utf8(single_line(p.text)) + "\n";
	}
	write_file(placed_path(), out);
}

// 辞書を認識結果に適用します (長い語から順に置換)
static std::wstring apply_corrections(const std::vector<Correction>& dict, std::wstring text)
{
	std::vector<const Correction*> order;
	for (const Correction& r : dict)
	{
		order.push_back(&r);
	}
	std::sort(order.begin(), order.end(), [](const Correction* a, const Correction* b)
		{
			return a->from.size() > b->from.size();
		});
	for (const Correction* r : order)
	{
		size_t pos = 0;
		while ((pos = text.find(r->from, pos)) != std::wstring::npos)
		{
			text.replace(pos, r->from.size(), r->to);
			pos += r->to.size();
		}
	}
	return text;
}

static bool only_punct(const std::wstring& s)
{
	for (wchar_t c : s)
	{
		if (!(c == L' ' || c == 0x3000 || c == 0x3001 || c == 0x3002 || c == L',' || c == L'.' || c == L'!' || c == L'?' || c == 0xFF01 || c == 0xFF1F))
		{
			return false;
		}
	}
	return true;
}

// 認識結果(a)と修正後(b)の差分から、置換辞書用の組を作ります
static bool make_pair_from_diff(const std::wstring& a, const std::wstring& b, std::wstring& from, std::wstring& to)
{
	if (a == b)
	{
		return false;
	}
	size_t p = 0;
	while (p < a.size() && p < b.size() && a[p] == b[p])
	{
		p++;
	}
	size_t ra = a.size();
	size_t rb = b.size();
	while (ra > p && rb > p && a[ra - 1] == b[rb - 1])
	{
		ra--;
		rb--;
	}
	// 認識結果側が2文字未満の場合は前後の文字を足して文脈にする
	size_t l = p;
	bool right = true;
	while ((ra - l) < 2)
	{
		if (right && ra < a.size())
		{
			ra++;
			rb++;
		}
		else if (l > 0)
		{
			l--;
		}
		else if (ra < a.size())
		{
			ra++;
			rb++;
		}
		else
		{
			break;
		}
		right = !right;
	}
	from = a.substr(l, ra - l);
	to = b.substr(l, rb - l);
	if (from.empty() || from == to || only_punct(from) || from.size() > 30 || to.size() > 30)
	{
		return false;
	}
	return true;
}

// Whisperへのヒント(initial prompt)に使う語彙を作ります
static std::string build_prompt(const std::vector<Correction>& dict)
{
	std::vector<const Correction*> order;
	for (const Correction& r : dict)
	{
		if (r.to.size() >= 2)
		{
			order.push_back(&r);
		}
	}
	std::sort(order.begin(), order.end(), [](const Correction* a, const Correction* b)
		{
			return a->count > b->count;
		});
	std::wstring p;
	int n = 0;
	for (const Correction* r : order)
	{
		if (n >= 30)
		{
			break;
		}
		if (!p.empty())
		{
			p += L"、";
		}
		p += r->to;
		n++;
	}
	return to_utf8(p);
}

//---------------------------------------------------------------------
//	テロップのエイリアス作成
//---------------------------------------------------------------------
static std::string build_alias(const std::string& tmpl, const std::wstring& text)
{
	std::string t = to_utf8(single_line(text));
	if (!tmpl.empty())
	{
		// エイリアスの「テキスト=」の行だけを、認識した字幕に差し替える (見た目の設定はそのまま使う)
		std::string out;
		bool header = true;
		bool replaced = false;
		size_t pos = 0;
		while (pos < tmpl.size())
		{
			size_t e = tmpl.find('\n', pos);
			if (e == std::string::npos)
			{
				e = tmpl.size();
			}
			std::string line = tmpl.substr(pos, e - pos);
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}
			pos = e + 1;
			if (line.compare(0, 8, "[Object.") == 0)
			{
				header = false;
			}
			if (header && line.compare(0, 6, "frame=") == 0)
			{
				continue;
			}
			if (line.compare(0, 13, u8"テキスト=") == 0)
			{
				line = std::string(u8"テキスト=") + t;
				replaced = true;
			}
			out += line + "\n";
		}
		if (replaced)
		{
			return out;
		}
	}
	return std::string(u8"[Object]\n[Object.0]\neffect.name=テキスト\nサイズ=100\n文字色=ffffff\nテキスト=") + t
		+ u8"\n[Object.1]\neffect.name=標準描画\n[Object.2]\neffect.name=縁取り\nサイズ=6\n縁色=000000\n";
}

//---------------------------------------------------------------------
//	音声の取得 (シーンの音声を16kHzモノラルに変換)
//---------------------------------------------------------------------
struct Capture
{
	int start_frame = 0;
	int rate = 30;
	int scale = 1;
	int64_t total = 0;
	std::vector<float> pcm;
	// 声の抽出用に、元のサンプリングレートのステレオ音声も保持する
	bool keep_native = false;
	int native_rate = 48000;
	int64_t native_total = 0;
	std::vector<float> nl;
	std::vector<float> nr;
};

static void on_audio(void* param, int frame, const float* b0, const float* b1, int n)
{
	Capture* c = (Capture*)param;
	if (n <= 0 || !b0)
	{
		return;
	}
	int64_t k = (int64_t)frame - c->start_frame;
	if (k < 0)
	{
		return;
	}
	if (c->keep_native)
	{
		int64_t nos = k * c->native_rate * c->scale / c->rate;
		int64_t noe = (k + 1) * c->native_rate * c->scale / c->rate;
		int64_t cnt = std::min<int64_t>(n, noe - nos);
		if (nos >= 0 && cnt > 0 && nos + cnt <= c->native_total)
		{
			for (int64_t j = 0; j < cnt; j++)
			{
				c->nl[(size_t)(nos + j)] = b0[j];
				c->nr[(size_t)(nos + j)] = b1 ? b1[j] : b0[j];
			}
		}
	}
	int64_t os = k * 16000LL * c->scale / c->rate;
	int64_t oe = (k + 1) * 16000LL * c->scale / c->rate;
	if (oe > c->total)
	{
		oe = c->total;
	}
	int64_t len = oe - os;
	for (int64_t i = 0; i < len; i++)
	{
		int a = (int)(i * n / len);
		int b = (int)((i + 1) * n / len);
		if (b <= a)
		{
			b = a + 1;
		}
		if (b > n)
		{
			b = n;
		}
		double sum = 0;
		for (int j = a; j < b; j++)
		{
			sum += b1 ? (b0[j] + b1[j]) * 0.5f : b0[j];
		}
		c->pcm[(size_t)(os + i)] = (float)(sum / (b - a));
	}
}

//---------------------------------------------------------------------
//	認識処理 (別スレッド)
//---------------------------------------------------------------------
struct Settings
{
	bool use_vad = true;
	float vad_threshold = 0.2f;
	int vad_min_silence_ms = 200;
	float vad_max_speech_s = 10.0f;
	int vad_pad_ms = 300;
	bool normalize = true;
	float target_rms = 0.1f;
	float highpass_hz = 100.0f;
	float logprob_thold = -2.0f;
	float no_speech_thold = 0.9f;
	int max_chars = 24;
	float split_gap_s = 0.3f;
	bool split_by_speech = true;
	int gpu_device = 0;
	bool separate = true;
	bool separate_gpu = true;
	int ort_device = 0;
	float ignore_no_speech = 0.3f;
};

struct Target
{
	int layer = 0;		// オブジェクトのレイヤー
	int obj_start = 0;	// オブジェクト自体の開始フレーム (オブジェクトを探し直す目印)
	int start = 0;		// 対象の開始フレーム (範囲選択で切り詰めた後)
	int end = 0;		// 対象の終了フレーム(含まない)
};

struct RunParams
{
	std::vector<Target> targets;	// 文字起こし対象のオブジェクト (選択レイヤー上)
	int layer = 0;
	int start_frame = 0;
	int end_frame = 0;		// 含まない
	int rate = 30;
	int scale = 1;
	int sample_rate = 48000;
	std::wstring model_path;
	std::wstring vad_path;
	std::string prompt;
	std::vector<Correction> dict;
	std::vector<std::wstring> ignore;	// 除外する幻覚フレーズ
	Settings st;
	std::wstring debug_wav;				// 取得した音声の確認用ファイル
	std::wstring debug_txt;				// 認識結果の詳細(確認用)
	HWND hwnd = nullptr;
};

static std::wstring ignore_path()
{
	return g_data_dir + L"\\ignore.txt";
}

// 除外リスト(ignore.txt)を読み込みます。無ければ既定の内容で作成します
static std::vector<std::wstring> load_ignore()
{
	std::vector<std::wstring> list;
	std::string data;
	if (!read_file(ignore_path(), data))
	{
		make_dirs(g_data_dir);
		const char* def =
			u8"# 音声認識が無音や環境音で出してしまう定型文のリストです (1行1フレーズ)\n"
			u8"# 句読点を除いて完全に一致した字幕だけを除外します。実際に話す言葉は消してください\n"
			u8"ご視聴ありがとうございました\n"
			u8"ご視聴ありがとうございます\n"
			u8"ご清聴ありがとうございました\n"
			u8"最後までご視聴いただきありがとうございました\n";
		write_file(ignore_path(), def);
		data = def;
	}
	for (const std::wstring& line : split_lines(data))
	{
		if (!line.empty() && line[0] != L'#')
		{
			list.push_back(line);
		}
	}
	return list;
}

static std::wstring strip_punct(const std::wstring& s)
{
	size_t b = 0;
	size_t e = s.size();
	auto is_p = [](wchar_t c)
		{
			return c == L' ' || c == 0x3000 || c == 0x3001 || c == 0x3002 || c == L',' || c == L'.' || c == L'!' || c == L'?' || c == 0xFF01 || c == 0xFF1F || c == L'\n' || c == L'\r' || c == L'\t';
		};
	while (b < e && is_p(s[b]))
	{
		b++;
	}
	while (e > b && is_p(s[e - 1]))
	{
		e--;
	}
	return s.substr(b, e - b);
}

static bool is_ignored(const std::wstring& text, const std::vector<std::wstring>& list)
{
	std::wstring t = strip_punct(text);
	for (const std::wstring& w : list)
	{
		if (t == strip_punct(w))
		{
			return true;
		}
	}
	return false;
}

// 取得した音声を確認用のWAVファイルに保存します (16kHz モノラル 16bit)
static void write_debug_wav(const std::wstring& path, const std::vector<float>& pcm, int64_t total)
{
	if (path.empty() || total <= 0 || total > 16000LL * 60 * 30)
	{
		return;
	}
	std::vector<int16_t> d((size_t)total);
	for (int64_t i = 0; i < total; i++)
	{
		float v = std::max(-1.0f, std::min(1.0f, pcm[(size_t)i]));
		d[(size_t)i] = (int16_t)(v * 32767.0f);
	}
	uint32_t data_bytes = (uint32_t)(total * 2);
	uint32_t riff = 36 + data_bytes;
	uint32_t fmt_size = 16;
	uint16_t format = 1;
	uint16_t channels = 1;
	uint32_t rate = 16000;
	uint32_t byte_rate = 32000;
	uint16_t block = 2;
	uint16_t bits = 16;
	FILE* fp = _wfopen(path.c_str(), L"wb");
	if (!fp)
	{
		return;
	}
	fwrite("RIFF", 1, 4, fp);
	fwrite(&riff, 4, 1, fp);
	fwrite("WAVEfmt ", 1, 8, fp);
	fwrite(&fmt_size, 4, 1, fp);
	fwrite(&format, 2, 1, fp);
	fwrite(&channels, 2, 1, fp);
	fwrite(&rate, 4, 1, fp);
	fwrite(&byte_rate, 4, 1, fp);
	fwrite(&block, 2, 1, fp);
	fwrite(&bits, 2, 1, fp);
	fwrite("data", 1, 4, fp);
	fwrite(&data_bytes, 4, 1, fp);
	fwrite(d.data(), 2, d.size(), fp);
	fclose(fp);
}

static std::wstring settings_path()
{
	return g_data_dir + L"\\settings.txt";
}

// 設定ファイル(settings.txt)を読み込みます。無ければ既定の内容で作成します
static Settings load_settings()
{
	Settings st;
	std::string data;
	if (!read_file(settings_path(), data))
	{
		make_dirs(g_data_dir);
		const char* def =
			u8"# テロップ自動生成の設定 (key=value / 変更後は次の文字起こしから反映)\n"
			u8"# vad: 無音検出を使うか (1=使う 0=使わない)\n"
			u8"vad=1\n"
			u8"# vad_threshold: 話し声と判定する基準。小さいほど拾いやすい (0.1~0.5)\n"
			u8"vad_threshold=0.2\n"
			u8"# vad_min_silence_ms: この長さ以上の無音で区切る\n"
			u8"vad_min_silence_ms=200\n"
			u8"# vad_max_speech_s: 1区間の最大秒数。長い区間は認識の取りこぼしが出やすいので短めに\n"
			u8"vad_max_speech_s=10\n"
			u8"# vad_pad_ms: 話し声の前後に足す余白\n"
			u8"vad_pad_ms=300\n"
			u8"# normalize: 音量を自動で揃えるか (1=する 0=しない)\n"
			u8"normalize=1\n"
			u8"# target_rms: 揃える音量の目安 (0.05~0.2)\n"
			u8"target_rms=0.1\n"
			u8"# highpass_hz: これより低い音(爆発音・低音のBGM)を弱める。0で無効\n"
			u8"highpass_hz=100\n"
			u8"# logprob_thold: 小さいほど自信のない認識結果も採用する\n"
			u8"logprob_thold=-2.0\n"
			u8"# no_speech_thold: 大きいほど「話していない」と判断されにくい (0~1)\n"
			u8"no_speech_thold=0.9\n"
			u8"# max_chars: 1つの字幕の最大文字数\n"
			u8"max_chars=24\n"
			u8"# split_gap_s: 話し声の間にこの秒数以上の間があれば、字幕を分けて表示時間を詰める\n"
			u8"split_gap_s=0.3\n"
			u8"# split_by_speech: 話し声の区間に合わせて字幕の位置を詰める (1=する 0=しない。0なら認識結果の時刻をそのまま使う)\n"
			u8"split_by_speech=1\n"
			u8"# gpu_device: 使うGPUの番号 (GPU対応でビルドした場合のみ有効。RTX搭載PCでIntel内蔵GPUが選ばれる場合は 1 などに変える)\n"
			u8"gpu_device=0\n"
			u8"# separate: 声の抽出(BGMや効果音を除く)の初期値 (1=オン 0=オフ。画面のチェックでも切り替え可能)\n"
			u8"separate=1\n"
			u8"# separate_gpu: 声の抽出でGPU(DirectML)を使うか (1=使う 0=CPUのみ)\n"
			u8"separate_gpu=1\n"
			u8"# ort_device: 声の抽出で使うGPUの番号 (Intel内蔵GPUが選ばれてしまう場合は 1 などに変える)\n"
			u8"ort_device=0\n"
			u8"# ignore_no_speech: ignore.txtと一致した字幕を、無音確率がこの値より高いときだけ除外する\n"
			u8"ignore_no_speech=0.3\n";
		write_file(settings_path(), def);
		return st;
	}
	for (const std::wstring& line : split_lines(data))
	{
		if (line.empty() || line[0] == L'#')
		{
			continue;
		}
		size_t eq = line.find(L'=');
		if (eq == std::wstring::npos)
		{
			continue;
		}
		std::wstring k = trim_text(line.substr(0, eq));
		std::wstring v = trim_text(line.substr(eq + 1));
		double d = wcstod(v.c_str(), nullptr);
		if (k == L"vad")
		{
			st.use_vad = d != 0;
		}
		else if (k == L"vad_threshold")
		{
			st.vad_threshold = (float)d;
		}
		else if (k == L"vad_min_silence_ms")
		{
			st.vad_min_silence_ms = (int)d;
		}
		else if (k == L"vad_max_speech_s")
		{
			st.vad_max_speech_s = (float)d;
		}
		else if (k == L"vad_pad_ms")
		{
			st.vad_pad_ms = (int)d;
		}
		else if (k == L"normalize")
		{
			st.normalize = d != 0;
		}
		else if (k == L"target_rms")
		{
			st.target_rms = (float)d;
		}
		else if (k == L"highpass_hz")
		{
			st.highpass_hz = (float)d;
		}
		else if (k == L"logprob_thold")
		{
			st.logprob_thold = (float)d;
		}
		else if (k == L"no_speech_thold")
		{
			st.no_speech_thold = (float)d;
		}
		else if (k == L"max_chars")
		{
			st.max_chars = (int)d;
		}
		else if (k == L"split_by_speech")
		{
			st.split_by_speech = d != 0;
		}
		else if (k == L"separate")
		{
			st.separate = d != 0;
		}
		else if (k == L"separate_gpu")
		{
			st.separate_gpu = d != 0;
		}
		else if (k == L"ort_device")
		{
			st.ort_device = (int)d;
		}
		else if (k == L"gpu_device")
		{
			st.gpu_device = (int)d;
		}
		else if (k == L"split_gap_s")
		{
			st.split_gap_s = (float)d;
		}
		else if (k == L"ignore_no_speech")
		{
			st.ignore_no_speech = (float)d;
		}
	}
	if (st.max_chars < 6)
	{
		st.max_chars = 6;
	}
	return st;
}

// 認識しやすいように音声を整えます (低音カット → 音量を揃える)
static std::wstring preprocess_audio(std::vector<float>& pcm, int64_t total, const Settings& st)
{
	std::wstring info;
	if (total <= 0)
	{
		return info;
	}
	const double fs = 16000.0;
	if (st.highpass_hz > 10.0f)
	{
		const double pi = 3.14159265358979323846;
		double w0 = 2.0 * pi * st.highpass_hz / fs;
		double alpha = std::sin(w0) / (2.0 * 0.70710678);
		double c = std::cos(w0);
		double b0 = (1.0 + c) / 2.0;
		double b1 = -(1.0 + c);
		double b2 = (1.0 + c) / 2.0;
		double a0 = 1.0 + alpha;
		double a1 = -2.0 * c;
		double a2 = 1.0 - alpha;
		b0 /= a0;
		b1 /= a0;
		b2 /= a0;
		a1 /= a0;
		a2 /= a0;
		double x1 = 0;
		double x2 = 0;
		double y1 = 0;
		double y2 = 0;
		for (int64_t i = 0; i < total; i++)
		{
			double x0 = pcm[(size_t)i];
			double y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1;
			x1 = x0;
			y2 = y1;
			y1 = y0;
			pcm[(size_t)i] = (float)y0;
		}
	}
	if (st.normalize)
	{
		// 20ms ごとの音量を調べ、上位10%付近の音量が目標に近づくように増幅する
		const int64_t win = 320;
		std::vector<float> levels;
		for (int64_t i = 0; i + win <= total; i += win)
		{
			double sq = 0;
			for (int64_t j = 0; j < win; j++)
			{
				double v = pcm[(size_t)(i + j)];
				sq += v * v;
			}
			float r = (float)std::sqrt(sq / (double)win);
			if (r > 1e-5f)
			{
				levels.push_back(r);
			}
		}
		if (levels.size() > 10)
		{
			size_t k = levels.size() * 9 / 10;
			std::nth_element(levels.begin(), levels.begin() + k, levels.end());
			double ref = levels[k];
			double gain = st.target_rms / std::max(ref, 1e-6);
			gain = std::min(30.0, std::max(0.5, gain));
			for (int64_t i = 0; i < total; i++)
			{
				double v = pcm[(size_t)i] * gain;
				double a = std::fabs(v);
				if (a > 0.8)
				{
					a = 0.8 + 0.2 * std::tanh((a - 0.8) / 0.2);
					v = (v < 0) ? -a : a;
				}
				pcm[(size_t)i] = (float)v;
			}
			char b[64];
			snprintf(b, sizeof(b), "音量x%.1f", gain);
			info = from_utf8(b);
		}
	}
	return info;
}

static void post_status(HWND hwnd, const std::wstring& m)
{
	PostMessageW(hwnd, WM_APP_STATUS, 0, (LPARAM) new std::wstring(m));
}

// 長いテキストを句読点などで分割して追加します
static void push_split(std::vector<Row>& out, int start, int end, std::wstring text, const std::vector<Correction>& dict)
{
	text = trim_text(text);
	if (text.empty())
	{
		return;
	}
	int total_len = (int)text.size();
	int total_frames = std::max(1, end - start);
	int base = start;
	while (true)
	{
		size_t cut = text.size();
		if (text.size() > MAX_CHARS)
		{
			cut = MAX_CHARS;
			for (size_t i = MAX_CHARS; i > MAX_CHARS / 2; i--)
			{
				wchar_t c = text[i - 1];
				if (c == 0x3001 || c == 0x3002 || c == 0xFF01 || c == 0xFF1F)
				{
					cut = i;
					break;
				}
			}
		}
		std::wstring piece = trim_text(text.substr(0, cut));
		std::wstring rest = (cut < text.size()) ? text.substr(cut) : std::wstring();
		int consumed = total_len - (int)rest.size();
		int piece_end = (rest.empty()) ? end : start + (int)((int64_t)total_frames * consumed / total_len);
		if (piece_end <= base)
		{
			piece_end = base + 1;
		}
		if (!piece.empty())
		{
			Row r;
			r.start = base;
			r.end = piece_end;
			r.text = apply_corrections(dict, piece);
			r.raw = r.text;
			out.push_back(r);
		}
		base = piece_end;
		if (rest.empty())
		{
			break;
		}
		text = rest;
	}
}

struct Span
{
	double a = 0;
	double b = 0;
};

// 認識された字幕の時間範囲に重なる話し声の区間を、間の短いものどうしでまとめて返します
static std::vector<Span> speech_groups(const std::vector<Span>& vad, double t0, double t1, double gap, double tighten)
{
	std::vector<Span> g;
	for (const Span& v : vad)
	{
		double a = std::max(v.a, t0);
		double b = std::min(v.b, t1);
		if (b <= a)
		{
			continue;
		}
		if (!g.empty() && a - g.back().b < gap)
		{
			g.back().b = std::max(g.back().b, b);
		}
		else
		{
			Span sp;
			sp.a = a;
			sp.b = b;
			g.push_back(sp);
		}
	}
	// VADの余白分を詰めて、表示時間を実際の発話に近づける
	for (Span& sp : g)
	{
		if (tighten > 0 && sp.b - sp.a > tighten * 2 + 0.3)
		{
			sp.a += tighten;
			sp.b -= tighten;
		}
	}
	return g;
}

// 音声のエネルギーから、話している区間を求めます (声だけの録音のように、無音との差がはっきりしている場合だけ)
static bool energy_spans(const std::vector<float>& pcm, int64_t total, double gap, std::vector<Span>& out)
{
	out.clear();
	const int64_t win = 320;	// 20ms
	std::vector<float> r;
	for (int64_t i = 0; i + win <= total; i += win)
	{
		double sq = 0;
		for (int64_t j = 0; j < win; j++)
		{
			double v = pcm[(size_t)(i + j)];
			sq += v * v;
		}
		r.push_back((float)std::sqrt(sq / (double)win));
	}
	if (r.size() < 50)
	{
		return false;
	}
	std::vector<float> sorted = r;
	std::sort(sorted.begin(), sorted.end());
	float floor_level = sorted[sorted.size() / 10];
	float p95 = sorted[sorted.size() * 95 / 100];
	// 無音との差が小さい(BGMなどが常に鳴っている)場合は、エネルギーでは判断しない
	if (p95 < 0.01f || floor_level > p95 * 0.06f)
	{
		return false;
	}
	float thr = std::max(floor_level * 5.0f, p95 * 0.10f);
	std::vector<Span> raw;
	bool in = false;
	double a = 0;
	float peak = 0;
	std::vector<float> peaks;
	for (size_t k = 0; k <= r.size(); k++)
	{
		double t = (double)k * 0.02;
		bool on = (k < r.size()) && r[k] >= thr;
		if (on)
		{
			if (!in)
			{
				in = true;
				a = t;
				peak = 0;
			}
			peak = std::max(peak, r[k]);
		}
		else if (in)
		{
			in = false;
			Span sp;
			sp.a = a;
			sp.b = t;
			raw.push_back(sp);
			peaks.push_back(peak);
		}
	}
	// 間の短いものは結合し、短すぎるもの・小さすぎる音は除外する
	std::vector<Span> merged;
	std::vector<float> merged_peaks;
	for (size_t i = 0; i < raw.size(); i++)
	{
		if (!merged.empty() && raw[i].a - merged.back().b < gap)
		{
			merged.back().b = raw[i].b;
			merged_peaks.back() = std::max(merged_peaks.back(), peaks[i]);
		}
		else
		{
			merged.push_back(raw[i]);
			merged_peaks.push_back(peaks[i]);
		}
	}
	for (size_t i = 0; i < merged.size(); i++)
	{
		if (merged[i].b - merged[i].a < 0.12 || merged_peaks[i] < p95 * 0.25f)
		{
			continue;
		}
		Span sp;
		sp.a = std::max(0.0, merged[i].a - 0.08);
		sp.b = merged[i].b + 0.15;
		out.push_back(sp);
	}
	return !out.empty();
}

// 分割位置を、近くの句読点に合わせます
static size_t snap_cut(const std::wstring& text, size_t cut, size_t min_pos)
{
	for (size_t d = 0; d <= 3; d++)
	{
		for (int sign = -1; sign <= 1; sign += 2)
		{
			long long idx = (long long)cut + sign * (long long)d;
			if (idx <= (long long)min_pos || idx >= (long long)text.size())
			{
				continue;
			}
			wchar_t c = text[(size_t)idx - 1];
			if (c == 0x3001 || c == 0x3002 || c == 0xFF01 || c == 0xFF1F)
			{
				return (size_t)idx;
			}
		}
	}
	return cut;
}

// 字幕を、話し声の区間ごとに分けて追加します (間があれば別々の字幕にして、無音の間は表示しない)
static void push_by_speech(std::vector<Row>& out, const std::wstring& text_in, double t0, double t1, const std::vector<Span>& vad,
	const Settings& st, double tighten, int base_frame, int rate, int scale, const std::vector<Correction>& dict)
{
	auto to_frame = [&](double sec)
		{
			return base_frame + (int)(sec * rate / scale + 0.5);
		};
	std::wstring text = trim_text(text_in);
	if (text.empty())
	{
		return;
	}
	Span whole;
	whole.a = t0;
	whole.b = t1;
	std::vector<Span> g;
	if (st.split_by_speech && !vad.empty())
	{
		g = speech_groups(vad, t0, t1, st.split_gap_s, tighten);
	}
	// 話し声の区間が信用できない場合は、認識結果の時刻をそのまま使う
	if (!g.empty())
	{
		double total_speech = 0;
		for (const Span& sp : g)
		{
			total_speech += sp.b - sp.a;
		}
		bool too_many = g.size() > 3;
		bool sparse = (double)text.size() < total_speech * 2.0;			// 話している時間のわりに文字が少ない (取りこぼし)
		if (too_many || sparse)
		{
			g.clear();
		}
	}
	if (g.empty())
	{
		g.push_back(whole);
	}
	// 文字数に対して区間が多すぎる場合は、間の近いものから結合する
	while (g.size() > 1 && text.size() < g.size() * 2)
	{
		size_t best = 0;
		double best_gap = 1e30;
		for (size_t i = 0; i + 1 < g.size(); i++)
		{
			double gap = g[i + 1].a - g[i].b;
			if (gap < best_gap)
			{
				best_gap = gap;
				best = i;
			}
		}
		g[best].b = g[best + 1].b;
		g.erase(g.begin() + best + 1);
	}
	if (g.size() == 1)
	{
		int f0 = to_frame(g[0].a);
		int f1 = std::max(to_frame(g[0].b), f0 + 1);
		push_split(out, f0, f1, text, dict);
		return;
	}
	double total = 0;
	for (const Span& sp : g)
	{
		total += sp.b - sp.a;
	}
	size_t pos = 0;
	double cum = 0;
	for (size_t k = 0; k < g.size(); k++)
	{
		std::wstring piece;
		if (k + 1 == g.size())
		{
			piece = text.substr(pos);
		}
		else
		{
			cum += g[k].b - g[k].a;
			size_t cut = (size_t)((double)text.size() * cum / total + 0.5);
			cut = snap_cut(text, cut, pos);
			cut = std::max(cut, pos + 1);
			cut = std::min(cut, text.size() - (g.size() - 1 - k));
			piece = text.substr(pos, cut - pos);
			pos = cut;
		}
		int f0 = to_frame(g[k].a);
		int f1 = std::max(to_frame(g[k].b), f0 + 1);
		push_split(out, f0, f1, piece, dict);
	}
}

// ファイルを探すフォルダ (プラグインのあるフォルダ、その下の TelopGenerator、データフォルダ)
static std::vector<std::wstring> search_dirs()
{
	std::vector<std::wstring> dirs;
	dirs.push_back(g_dll_dir);
	dirs.push_back(g_dll_dir + L"\\TelopGenerator");
	dirs.push_back(g_data_dir);
	return dirs;
}

// 声の抽出に使うONNXモデル (models フォルダ内の最初の .onnx)
static std::wstring find_onnx_model()
{
	for (const std::wstring& base : search_dirs())
	{
		for (int k = 0; k < 2; k++)
		{
			std::wstring dir = (k == 0) ? (base + L"\\models") : base;
			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW((dir + L"\\*.onnx").c_str(), &fd);
			if (h != INVALID_HANDLE_VALUE)
			{
				std::wstring path = dir + L"\\" + fd.cFileName;
				FindClose(h);
				return path;
			}
		}
	}
	return std::wstring();
}

// onnxruntime.dll の場所 (見つからなければ空)
static std::wstring find_ort_dll()
{
	for (const std::wstring& base : search_dirs())
	{
		std::wstring a = base + L"\\onnxruntime\\onnxruntime.dll";
		if (file_exists(a))
		{
			return a;
		}
		std::wstring b = base + L"\\onnxruntime.dll";
		if (file_exists(b))
		{
			return b;
		}
	}
	return std::wstring();
}

// ggml が認識しているデバイス (GPUが使われるかの確認用)
static std::string describe_devices()
{
	std::string out;
	size_t n = ggml_backend_dev_count();
	for (size_t i = 0; i < n; i++)
	{
		ggml_backend_dev_t d = ggml_backend_dev_get(i);
		enum ggml_backend_dev_type t = ggml_backend_dev_type(d);
		const char* kind = (t == GGML_BACKEND_DEVICE_TYPE_GPU) ? "GPU" : (t == GGML_BACKEND_DEVICE_TYPE_IGPU) ? "内蔵GPU" : (t == GGML_BACKEND_DEVICE_TYPE_CPU) ? "CPU" : "その他";
		out += std::string("  [") + kind + "] " + ggml_backend_dev_name(d) + " : " + ggml_backend_dev_description(d) + "\n";
	}
	if (out.empty())
	{
		out = "  (デバイスなし)\n";
	}
	return out;
}

// 取得した音声から声だけを取り出し、cap.pcm (16kHz モノラル) を置き換えます。中止された場合は false
static bool run_separation(const RunParams& p, Capture& cap, const std::wstring& model, const std::wstring& dll, std::wstring& note)
{
	DWORD t_start = GetTickCount();
	g_step = "声の抽出";
	post_status(p.hwnd, L"声を抽出中 (BGMや効果音を除いています)...");
	std::vector<float> l = voicesep::resample(cap.nl, cap.native_rate, 44100);
	std::vector<float> r = voicesep::resample(cap.nr, cap.native_rate, 44100);
	cap.nl.clear();
	cap.nl.shrink_to_fit();
	cap.nr.clear();
	cap.nr.shrink_to_fit();

	voicesep::OrtRunner runner;
	std::string err;
	unsigned int hc = std::thread::hardware_concurrency();
	if (!runner.open(dll, model, p.st.separate_gpu, p.st.ort_device, (hc > 2) ? (int)hc - 2 : 1, err))
	{
		note += L" / 声の抽出を開始できませんでした: " + from_utf8(err);
		return true;
	}
	voicesep::Params prm;
	std::vector<float> vl;
	std::vector<float> vr;
	bool ok = voicesep::separate(l, r, vl, vr, prm, runner, [&](int done, int total)
		{
			PostMessageW(p.hwnd, WM_APP_PROGRESS, (WPARAM)(25 + 30LL * done / std::max(1, total)), 0);
			return !g_cancel.load();
		}, err);
	bool gpu = runner.using_gpu();
	runner.close();
	if (g_cancel)
	{
		return false;
	}
	if (!ok)
	{
		note += L" / 声の抽出に失敗しました: " + from_utf8(err);
		return true;
	}
	std::vector<float> mono(vl.size());
	for (size_t i = 0; i < mono.size(); i++)
	{
		mono[i] = (vl[i] + vr[i]) * 0.5f;
	}
	std::vector<float> m16 = voicesep::resample(mono, 44100, 16000);
	std::fill(cap.pcm.begin(), cap.pcm.end(), 0.0f);
	size_t n = std::min(m16.size(), cap.pcm.size());
	std::copy(m16.begin(), m16.begin() + n, cap.pcm.begin());
	DWORD sec = (GetTickCount() - t_start) / 1000;
	note += std::wstring(L" / 声を抽出(") + (gpu ? L"GPU" : L"CPU") + L"、" + std::to_wstring(sec) + L"秒)";
	g_prog_base = 55;
	return true;
}

static void worker_main(const RunParams& p)
{
	HWND hwnd = p.hwnd;
	Result* result = nullptr;
	std::wstring note;
	do
	{
		// 1. 音声の取得
		int frames = p.end_frame - p.start_frame;
		if (frames <= 0)
		{
			post_status(hwnd, L"対象の範囲にオブジェクトがありません");
			break;
		}
		Capture cap;
		cap.start_frame = p.start_frame;
		cap.rate = p.rate;
		cap.scale = p.scale;
		cap.total = (int64_t)frames * 16000LL * p.scale / p.rate;
		cap.pcm.assign((size_t)cap.total + 16000, 0.0f);
		g_prog_base = 25;
		std::wstring onnx_model = find_onnx_model();
		std::wstring ort_dll = find_ort_dll();
		bool want_sep = p.st.separate;
		bool sep_ok = want_sep && !onnx_model.empty() && !ort_dll.empty();
		if (want_sep && !sep_ok)
		{
			note += L" / 声の抽出をスキップしました (見つからないもの:";
			if (ort_dll.empty())
			{
				note += L" onnxruntime\\onnxruntime.dll";
			}
			if (onnx_model.empty())
			{
				note += L" models\\*.onnx";
			}
			note += L" / 探した場所: " + g_dll_dir + L")";
		}
		if (sep_ok)
		{
			cap.keep_native = true;
			cap.native_rate = p.sample_rate;
			cap.native_total = (int64_t)frames * p.sample_rate * p.scale / p.rate + p.sample_rate;
			cap.nl.assign((size_t)cap.native_total, 0.0f);
			cap.nr.assign((size_t)cap.native_total, 0.0f);
		}

		g_step = "音声の取得";
		const int BATCH = 300;
		bool ok = true;
		int rendered_objects = 0;
		int64_t sum_frames = 0;
		int64_t done_frames = 0;
		for (const Target& t : p.targets)
		{
			sum_frames += t.end - t.start;
		}
		struct EnqueueCtx
		{
			const Target* t = nullptr;
			int from = 0;
			int to = 0;
			Capture* cap = nullptr;
			int enqueued = 0;
			bool missing = false;
		};
		for (const Target& t : p.targets)
		{
			bool first = true;
			bool usable = true;
			for (int f = t.start; f < t.end && usable && ok; f += BATCH)
			{
				if (g_cancel)
				{
					break;
				}
				int to = std::min(t.end, f + BATCH);
				EnqueueCtx ec;
				ec.t = &t;
				ec.from = f;
				ec.to = to;
				ec.cap = &cap;
				// オブジェクトは、読み取りセクション内で探し直してから音声の取得を依頼する (保持したハンドルは使わない)
				bool called = g_edit->call_read_section_param(&ec, [](void* param, EDIT_SECTION* e)
					{
						EnqueueCtx* c = (EnqueueCtx*)param;
						OBJECT_HANDLE o = e->find_object(c->t->layer, c->t->obj_start);
						if (!o)
						{
							c->missing = true;
							return;
						}
						OBJECT_LAYER_FRAME lf = e->get_object_layer_frame(o);
						if (lf.start != c->t->obj_start)
						{
							c->missing = true;
							return;
						}
						for (int i = c->from; i < c->to; i++)
						{
							if (!g_edit->rendering_object_audio(o, i, true, c->cap, on_audio))
							{
								break;
							}
							c->enqueued++;
						}
					});
				if (!called || ec.missing)
				{
					ok = false;
					break;
				}
				g_edit->wait_rendering_task();
				if (ec.enqueued < to - f)
				{
					if (first && ec.enqueued == 0)
					{
						usable = false;	// 音声を持たないオブジェクト
					}
					else
					{
						ok = false;
					}
				}
				if (first && ec.enqueued > 0)
				{
					first = false;
					rendered_objects++;
				}
				done_frames += to - f;
				PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)(25LL * done_frames / std::max<int64_t>(1, sum_frames)), 0);
			}
			if (g_cancel || !ok)
			{
				break;
			}
		}
		if (!g_cancel && ok && rendered_objects == 0)
		{
			post_status(hwnd, L"レイヤー" + std::to_wstring(p.layer + 1) + L"に音声を持つオブジェクトがありません。文字起こししたいオブジェクトのあるレイヤーを選択してください");
			break;
		}
		if (g_cancel)
		{
			post_status(hwnd, L"中止しました");
			break;
		}
		if (!ok)
		{
			post_status(hwnd, L"音声を取得できませんでした (出力中などは実行できません)");
			break;
		}

		// 取得した音声のレベルを確認 (無音の音声を認識させると、存在しない言葉が出力されやすい)
		double peak = 0;
		double sq = 0;
		for (int64_t i = 0; i < cap.total; i++)
		{
			double v = cap.pcm[(size_t)i];
			peak = std::max(peak, std::fabs(v));
			sq += v * v;
		}
		double rms = (cap.total > 0) ? std::sqrt(sq / (double)cap.total) : 0.0;
		char lv[128];
		snprintf(lv, sizeof(lv), "音声レベル 最大%.3f 平均%.4f", peak, rms);
		note = from_utf8(lv);
		if (peak < 0.001)
		{
			post_status(hwnd, L"取得した音声が無音です。音声オブジェクトの有無・ミュート・音量を確認してください");
			break;
		}
		if (p.st.use_vad && p.vad_path.empty())
		{
			note += L" / 無音検出モデル(VAD)が無いため、話していない部分の誤認識が出やすくなります";
		}
		if (sep_ok)
		{
			if (!run_separation(p, cap, onnx_model, ort_dll, note))
			{
				post_status(hwnd, L"中止しました");
				break;
			}
		}
		std::wstring pre = preprocess_audio(cap.pcm, cap.total, p.st);
		if (!pre.empty())
		{
			note += L" / " + pre;
		}
		write_debug_wav(p.debug_wav, cap.pcm, cap.total);

		// 2. モデルの読み込み
		g_step = "モデルの読み込み";
		post_status(hwnd, L"モデルを読み込み中...");
		std::string model_u8 = to_utf8(p.model_path);
		whisper_context_params cp = whisper_context_default_params();
		cp.use_gpu = true;
		cp.gpu_device = p.st.gpu_device;
		whisper_context* ctx = whisper_init_from_file_with_params(model_u8.c_str(), cp);
		if (!ctx)
		{
			post_status(hwnd, L"モデルの読み込みに失敗しました: " + p.model_path);
			break;
		}

		// 3. 認識
		g_step = "文字起こし";
		post_status(hwnd, L"文字起こし中...");
		whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
		wp.beam_search.beam_size = 5;
		wp.language = "ja";
		wp.detect_language = false;
		wp.translate = false;
		wp.no_context = true;
		wp.print_progress = false;
		wp.print_realtime = false;
		wp.print_special = false;
		wp.print_timestamps = false;
		wp.suppress_blank = true;
		wp.suppress_nst = true;
		wp.logprob_thold = p.st.logprob_thold;
		wp.no_speech_thold = p.st.no_speech_thold;
		unsigned int hc = std::thread::hardware_concurrency();
		wp.n_threads = (hc > 2) ? (int)hc - 2 : 1;
		wp.initial_prompt = p.prompt.empty() ? nullptr : p.prompt.c_str();
		std::string vad_u8 = to_utf8(p.vad_path);
		if (!p.vad_path.empty())
		{
			wp.vad = true;
			wp.vad_model_path = vad_u8.c_str();
			// BGMや効果音と重なった声も拾えるように、検出を甘めにする
			wp.vad_params = whisper_vad_default_params();
			wp.vad_params.threshold = p.st.vad_threshold;
			wp.vad_params.min_speech_duration_ms = 150;
			wp.vad_params.min_silence_duration_ms = p.st.vad_min_silence_ms;
			wp.vad_params.max_speech_duration_s = p.st.vad_max_speech_s;
			wp.vad_params.speech_pad_ms = p.st.vad_pad_ms;
		}
		wp.progress_callback = [](whisper_context*, whisper_state*, int progress, void* user)
			{
				PostMessageW((HWND)user, WM_APP_PROGRESS, (WPARAM)(g_prog_base + progress * (100 - g_prog_base) / 100), 0);
			};
		wp.progress_callback_user_data = hwnd;
		wp.abort_callback = [](void*)
			{
				return g_cancel.load();
			};
		wp.abort_callback_user_data = nullptr;

		DWORD t_whisper = GetTickCount();
		int rc = whisper_full(ctx, wp, cap.pcm.data(), (int)std::min<int64_t>(cap.total, INT32_MAX));
		note += L" / 文字起こし" + std::to_wstring((GetTickCount() - t_whisper) / 1000) + L"秒";
		if (g_cancel)
		{
			whisper_free(ctx);
			post_status(hwnd, L"中止しました");
			break;
		}
		if (rc != 0)
		{
			whisper_free(ctx);
			post_status(hwnd, L"文字起こしに失敗しました");
			break;
		}

		// 4. 結果の変換
		g_step = "結果の変換";
		result = new Result();
		result->tb.rate = p.rate;
		result->tb.scale = p.scale;
		int n = whisper_full_n_segments(ctx);
		int skipped = 0;
		std::string dump;
		std::vector<Span> vad_spans;
		std::vector<Span> en_spans;
		bool clean = p.st.split_by_speech && energy_spans(cap.pcm, cap.total, p.st.split_gap_s, en_spans);
		if (!p.vad_path.empty())
		{
			int vn = whisper_full_n_vad_segments(ctx);
			for (int i = 0; i < vn; i++)
			{
				Span sp;
				sp.a = whisper_full_get_vad_segment_t0(ctx, i) * 0.01;
				sp.b = whisper_full_get_vad_segment_t1(ctx, i) * 0.01;
				vad_spans.push_back(sp);
			}
		}
		// 話している区間が、どの字幕に属するかを決める (字幕の時刻は前後の無音まで広がっていることがあるため)
		std::vector<std::vector<size_t>> owned_idx((size_t)n);
		std::vector<int> share(en_spans.size(), 0);
		if (clean)
		{
			for (size_t si = 0; si < en_spans.size(); si++)
			{
				double sd = en_spans[si].b - en_spans[si].a;
				for (int i = 0; i < n; i++)
				{
					double a0 = whisper_full_get_segment_t0(ctx, i) * 0.01;
					double a1 = whisper_full_get_segment_t1(ctx, i) * 0.01;
					double ov = std::min(en_spans[si].b, a1) - std::max(en_spans[si].a, a0);
					if (ov > 0 && ov >= 0.5 * std::min(a1 - a0, sd))
					{
						owned_idx[(size_t)i].push_back(si);
						share[si]++;
					}
				}
			}
		}
		for (int i = 0; i < n; i++)
		{
			double t0 = whisper_full_get_segment_t0(ctx, i) * 0.01;
			double t1 = whisper_full_get_segment_t1(ctx, i) * 0.01;
			const char* txt = whisper_full_get_segment_text(ctx, i);
			std::wstring seg_text = from_utf8(txt ? txt : "");
			float no_speech = whisper_full_get_segment_no_speech_prob(ctx, i);
			char head[96];
			snprintf(head, sizeof(head), "[%d] %.2f-%.2f 無音確率%.2f ", i + 1, t0, t1, no_speech);
			dump += head;
			bool listed = is_ignored(seg_text, p.ignore);
			if (listed && no_speech > p.st.ignore_no_speech)
			{
				skipped++;
				dump += "除外(ignore.txtに一致し、無音の可能性が高い) " + to_utf8(seg_text) + "\n";
				continue;
			}
			dump += listed ? "採用(ignore.txtに一致するが話し声と判定) " : "採用 ";
			dump += to_utf8(seg_text) + "\n";
			std::vector<Span> owned;
			if (clean)
			{
				for (size_t si : owned_idx[(size_t)i])
				{
					Span sp = en_spans[si];
					if (share[si] > 1)
					{
						// 複数の字幕で共有している区間は、この字幕の範囲に区切る
						sp.a = std::max(sp.a, t0);
						sp.b = std::min(sp.b, t1);
					}
					if (sp.b > sp.a)
					{
						owned.push_back(sp);
					}
				}
				double total_speech = 0;
				for (const Span& sp : owned)
				{
					total_speech += sp.b - sp.a;
				}
				// 話している区間が無い、または話している時間に対して文字が多すぎる(1秒に12文字超)字幕は、幻覚とみなして除外
				if (owned.empty() || (double)trim_text(seg_text).size() > total_speech * 12.0 + 2.0)
				{
					skipped++;
					dump += "    -> 除外(話している区間が無い、または文字が多すぎる)\n";
					continue;
				}
				t0 = owned.front().a;
				t1 = owned.back().b;
			}
			size_t before = result->rows.size();
			push_by_speech(result->rows, seg_text, t0, t1, clean ? owned : vad_spans, p.st, clean ? 0.0 : 0.15, p.start_frame, p.rate, p.scale, p.dict);
			for (size_t k = before; k < result->rows.size(); k++)
			{
				const Row& pr = result->rows[k];
				char pos[96];
				snprintf(pos, sizeof(pos), "    -> 配置 %.2f-%.2f ", (double)(pr.start - p.start_frame) * p.scale / p.rate, (double)(pr.end - p.start_frame) * p.scale / p.rate);
				dump += pos;
				dump += to_utf8(pr.text) + "\n";
			}
		}
		int vad_n = p.vad_path.empty() ? -1 : whisper_full_n_vad_segments(ctx);
		whisper_free(ctx);
		{
			std::string head = "\xEF\xBB\xBF";
			head += "モデル: " + to_utf8(p.model_path) + "\n";
			head += std::string("実行環境: ") + whisper_print_system_info() + "\n";
			head += "ggmlのデバイス:\n" + describe_devices();
			head += std::string("声の抽出: ") + (want_sep ? "有効" : "無効") + " / onnxruntime.dll: " + (ort_dll.empty() ? std::string("見つからない") : to_utf8(ort_dll)) + " / モデル: " + (onnx_model.empty() ? std::string("見つからない") : to_utf8(onnx_model)) + "\n";
			head += "処理の記録: " + to_utf8(note) + "\n";
			head += std::string("VAD: ") + (vad_n >= 0 ? std::to_string(vad_n) + "区間" : std::string("なし")) + "\n";
			head += "認識されたセグメント: " + std::to_string(n) + "件\n";
			for (const Span& sp : vad_spans)
			{
				char vb[64];
				snprintf(vb, sizeof(vb), "  VAD区間 %.2f-%.2f\n", sp.a, sp.b);
				head += vb;
			}
			head += std::string("話し声の判定: ") + (clean ? "音量(声だけの録音と判断)" : "VAD") + "\n";
			for (const Span& sp : en_spans)
			{
				char eb[64];
				snprintf(eb, sizeof(eb), "  音量で検出した区間 %.2f-%.2f\n", sp.a, sp.b);
				head += eb;
			}
			head += "\n";
			write_file(p.debug_txt, head + dump);
		}
		if (vad_n >= 0)
		{
			note += L" / VAD区間" + std::to_wstring(vad_n) + L"個";
		}
		note += L" / 認識" + std::to_wstring(n) + L"件";

		// 隣り合う同じ字幕(認識の重複)は、後ろのものを除外
		{
			std::vector<Row> kept;
			int near_frames = (int)(0.5 * p.rate / p.scale);
			for (const Row& r : result->rows)
			{
				if (!kept.empty() && strip_punct(kept.back().text) == strip_punct(r.text) && r.start - kept.back().end <= near_frames)
				{
					skipped++;
					continue;
				}
				kept.push_back(r);
			}
			result->rows = kept;
		}

		// 同じ字幕が3つ以上連続する場合は、認識の暴走とみなして除外
		{
			std::vector<Row> kept;
			size_t i = 0;
			while (i < result->rows.size())
			{
				size_t j = i + 1;
				while (j < result->rows.size() && result->rows[j].text == result->rows[i].text)
				{
					j++;
				}
				if (j - i >= 3)
				{
					skipped += (int)(j - i);
				}
				else
				{
					for (size_t k = i; k < j; k++)
					{
						kept.push_back(result->rows[k]);
					}
				}
				i = j;
			}
			result->rows = kept;
		}
		if (skipped > 0)
		{
			note += L" / 話し声ではない可能性が高い" + std::to_wstring(skipped) + L"件を除外";
		}
		result->note = note;

		// 隣のテロップと重ならないように調整
		for (size_t i = 0; i + 1 < result->rows.size(); i++)
		{
			if (result->rows[i].end > result->rows[i + 1].start)
			{
				result->rows[i].end = std::max(result->rows[i].start + 1, result->rows[i + 1].start);
			}
		}
	} while (false);

	if (result)
	{
		PostMessageW(hwnd, WM_APP_DONE, 0, (LPARAM)result);
	}
	else
	{
		PostMessageW(hwnd, WM_APP_DONE, 0, 0);
	}
}

#ifdef _MSC_VER
static int write_crash(EXCEPTION_POINTERS* ep)
{
	char buf[512];
	void* addr = ep ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
	HMODULE mod = nullptr;
	char modname[260] = "";
	if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod))
	{
		GetModuleFileNameA(mod, modname, sizeof(modname));
	}
	snprintf(buf, sizeof(buf), "code=0x%08lX address=%p module=%s step=%s\r\n", (unsigned long)code, addr, modname, g_step);
	write_file(g_data_dir + L"\\crash.txt", buf);
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static void report_crash(HWND hwnd)
{
	post_status(hwnd, L"処理中に内部エラーが起きました。data\\TelopGenerator\\crash.txt を確認してください");
	PostMessageW(hwnd, WM_APP_DONE, 0, 0);
}

static DWORD WINAPI worker_thread_proc(LPVOID param)
{
	RunParams* rp = (RunParams*)param;
#ifdef _MSC_VER
	__try
	{
		worker_main(*rp);
	}
	__except (write_crash(GetExceptionInformation()))
	{
		report_crash(rp->hwnd);
	}
#else
	worker_main(*rp);
#endif
	delete rp;
	return 0;
}

static void join_worker()
{
	if (g_worker)
	{
		WaitForSingleObject(g_worker, INFINITE);
		CloseHandle(g_worker);
		g_worker = nullptr;
	}
}

//---------------------------------------------------------------------
//	ウィンドウの表示更新
//---------------------------------------------------------------------
static void set_status(const std::wstring& m)
{
	SetWindowTextW(g_status, m.c_str());
}

static int selected_index()
{
	return ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
}

static void refresh_list(int select = -1)
{
	g_updating = true;
	ListView_DeleteAllItems(g_list);
	for (size_t i = 0; i < g_rows.size(); i++)
	{
		const Row& r = g_rows[i];
		std::wstring num = std::to_wstring(i + 1);
		LVITEMW it = {};
		it.mask = LVIF_TEXT;
		it.iItem = (int)i;
		it.pszText = (LPWSTR)num.c_str();
		ListView_InsertItem(g_list, &it);
		std::wstring s = fmt_time(r.start);
		std::wstring e = fmt_time(r.end);
		ListView_SetItemText(g_list, (int)i, 1, (LPWSTR)s.c_str());
		ListView_SetItemText(g_list, (int)i, 2, (LPWSTR)e.c_str());
		ListView_SetItemText(g_list, (int)i, 3, (LPWSTR)r.text.c_str());
		ListView_SetCheckState(g_list, (int)i, r.check ? TRUE : FALSE);
	}
	if (select >= 0 && select < (int)g_rows.size())
	{
		ListView_SetItemState(g_list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(g_list, select, FALSE);
	}
	g_updating = false;
}

static void load_edit_boxes(int index)
{
	if (index < 0 || index >= (int)g_rows.size())
	{
		return;
	}
	const Row& r = g_rows[index];
	SetWindowTextW(g_edit_start, fmt_time(r.start).c_str());
	SetWindowTextW(g_edit_end, fmt_time(r.end).c_str());
	SetWindowTextW(g_edit_text, r.text.c_str());
}

static std::wstring get_text(HWND h)
{
	int n = GetWindowTextLengthW(h);
	std::wstring s(n + 1, L'\0');
	GetWindowTextW(h, &s[0], n + 1);
	s.resize(n);
	return s;
}

// 指定のフレームに再生バーを移動します
static void jump_to(int frame)
{
	if (!g_edit)
	{
		return;
	}
	g_edit->call_edit_section_param(&frame, [](void* param, EDIT_SECTION* edit)
		{
			edit->set_cursor_layer_frame(edit->info->layer, *(int*)param);
		});
}

//---------------------------------------------------------------------
//	ボタンの処理
//---------------------------------------------------------------------
//---------------------------------------------------------------------
//	テロップのエイリアス選択
//---------------------------------------------------------------------
static std::wstring selected_alias_path()
{
	return g_data_dir + L"\\selected_alias.txt";
}

static std::wstring alias_display_name(const std::wstring& rel)
{
	if (rel.empty())
	{
		return std::wstring(L"(標準のテロップ)");
	}
	std::wstring n = rel;
	if (n.size() > 7)
	{
		n.erase(n.size() - 7);
	}
	return n;
}

static int count_alias_edits(const std::wstring& rel);
static std::wstring alias_edit_key();

static void update_alias_label()
{
	if (g_alias_label)
	{
		std::wstring label = L"使用するエイリアス: " + alias_display_name(g_alias_rel);
		int n = count_alias_edits(alias_edit_key());
		if (n > 0)
		{
			label += L"  (編集 " + std::to_wstring(n) + L"項目)";
		}
		SetWindowTextW(g_alias_label, label.c_str());
	}
}

static void load_alias_selection()
{
	g_alias_rel.clear();
	std::string data;
	if (read_file(selected_alias_path(), data))
	{
		std::vector<std::wstring> lines = split_lines(data);
		if (!lines.empty() && file_exists(g_alias_dir + L"\\" + lines[0]))
		{
			g_alias_rel = lines[0];
		}
	}
	update_alias_label();
}

static void save_alias_selection()
{
	make_dirs(g_data_dir);
	write_file(selected_alias_path(), to_utf8(g_alias_rel));
}

// 標準のテロップ (エイリアスを選んでいないとき)
static std::string builtin_template()
{
	return std::string(u8"[Object]\n[Object.0]\neffect.name=テキスト\nサイズ=100\n文字色=ffffff\nテキスト=\n[Object.1]\neffect.name=標準描画\n[Object.2]\neffect.name=縁取り\nサイズ=6\n縁色=000000\n");
}

//---------------------------------------------------------------------
//	エイリアスの項目の編集 (元のエイリアスのファイルは変更せず、変更内容だけを保存して配置時に反映する)
//---------------------------------------------------------------------
struct AliasEdit
{
	std::wstring rel;		// エイリアス (標準のテロップは "*")
	std::wstring section;
	std::wstring key;
	std::wstring value;
};
static std::vector<AliasEdit> g_alias_edits;

struct AliasParam
{
	std::wstring section;
	std::wstring effect;
	std::wstring key;
	std::wstring value;
};

static std::wstring alias_edit_key()
{
	return g_alias_rel.empty() ? std::wstring(L"*") : g_alias_rel;
}

static std::wstring alias_edits_path()
{
	return g_data_dir + L"\\alias_edits.txt";
}

static void load_alias_edits()
{
	g_alias_edits.clear();
	std::string data;
	if (!read_file(alias_edits_path(), data))
	{
		return;
	}
	for (const std::wstring& line : split_lines(data))
	{
		std::vector<std::wstring> c = split_tab(line);
		if (c.size() >= 4)
		{
			AliasEdit e;
			e.rel = c[0];
			e.section = c[1];
			e.key = c[2];
			e.value = c[3];
			g_alias_edits.push_back(e);
		}
	}
}

static void save_alias_edits()
{
	make_dirs(g_data_dir);
	std::string out;
	for (const AliasEdit& e : g_alias_edits)
	{
		out += to_utf8(single_line(e.rel)) + "\t" + to_utf8(single_line(e.section)) + "\t" + to_utf8(single_line(e.key)) + "\t" + to_utf8(single_line(e.value)) + "\n";
	}
	write_file(alias_edits_path(), out);
}

static int count_alias_edits(const std::wstring& rel)
{
	int n = 0;
	for (const AliasEdit& e : g_alias_edits)
	{
		if (e.rel == rel)
		{
			n++;
		}
	}
	return n;
}

// エイリアスの内容から、編集できる項目を取り出す (テキストそのものと、フレーム情報は除く)
static void parse_alias_params(const std::string& data, std::vector<AliasParam>& out)
{
	out.clear();
	std::wstring section;
	std::wstring effect;
	for (const std::wstring& line : split_lines(data))
	{
		if (line.size() >= 2 && line[0] == L'[' && line.back() == L']')
		{
			section = line.substr(1, line.size() - 2);
			effect.clear();
			continue;
		}
		size_t eq = line.find(L'=');
		if (eq == std::wstring::npos || eq == 0)
		{
			continue;
		}
		std::wstring key = line.substr(0, eq);
		std::wstring value = line.substr(eq + 1);
		if (key == L"effect.name")
		{
			effect = value;
			continue;
		}
		if (key == L"frame" || key == L"layer" || key == L"テキスト" || section == L"Object")
		{
			continue;
		}
		AliasParam p;
		p.section = section;
		p.effect = effect;
		p.key = key;
		p.value = value;
		out.push_back(p);
	}
}

// 保存してある編集内容を、エイリアスの内容に反映する
static std::string apply_alias_edits(const std::string& tmpl, const std::wstring& rel)
{
	if (count_alias_edits(rel) == 0)
	{
		return tmpl;
	}
	std::string out;
	std::wstring section;
	size_t pos = 0;
	while (pos < tmpl.size())
	{
		size_t e = tmpl.find('\n', pos);
		if (e == std::string::npos)
		{
			e = tmpl.size();
		}
		std::string raw = tmpl.substr(pos, e - pos);
		pos = e + 1;
		if (!raw.empty() && raw.back() == '\r')
		{
			raw.pop_back();
		}
		std::wstring line = from_utf8(raw);
		if (line.size() >= 2 && line[0] == L'[' && line.back() == L']')
		{
			section = line.substr(1, line.size() - 2);
		}
		else
		{
			size_t eq = line.find(L'=');
			if (eq != std::wstring::npos && eq > 0)
			{
				std::wstring key = line.substr(0, eq);
				for (const AliasEdit& ed : g_alias_edits)
				{
					if (ed.rel == rel && ed.section == section && ed.key == key)
					{
						raw = to_utf8(key + L"=" + ed.value);
						break;
					}
				}
			}
		}
		out += raw + "\n";
	}
	return out;
}

// 編集前のエイリアスの内容 (選択したエイリアス → template.object → 標準)
static std::string base_template()
{
	std::string t;
	if (g_alias_rel.empty() || !read_file(g_alias_dir + L"\\" + g_alias_rel, t))
	{
		t.clear();
		if (!read_file(template_path(), t) || t.empty())
		{
			t = builtin_template();
		}
	}
	return t;
}

// 配置に使うエイリアスの内容 (編集内容を反映済み)
static std::string current_template()
{
	return apply_alias_edits(base_template(), alias_edit_key());
}

// テキストのオブジェクトを含むエイリアス(.object)を探す
static void scan_aliases(const std::wstring& dir, const std::wstring& rel, std::vector<std::wstring>& out, int depth)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		return;
	}
	do
	{
		std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..")
		{
			continue;
		}
		std::wstring r = rel.empty() ? name : (rel + L"\\" + name);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (depth < 5)
			{
				scan_aliases(dir + L"\\" + name, r, out, depth + 1);
			}
			continue;
		}
		if (name.size() <= 7)
		{
			continue;
		}
		std::wstring ext = name.substr(name.size() - 7);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
		if (ext != L".object")
		{
			continue;
		}
		std::string data;
		if (!read_file(dir + L"\\" + name, data))
		{
			continue;
		}
		const std::string key = u8"effect.name=テキスト";
		size_t pos = 0;
		bool found = false;
		while ((pos = data.find(key, pos)) != std::string::npos)
		{
			size_t end = pos + key.size();
			if (end >= data.size() || data[end] == '\r' || data[end] == '\n')
			{
				found = true;
				break;
			}
			pos = end;
		}
		if (found)
		{
			out.push_back(r);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

struct AliasDlg
{
	std::vector<std::wstring> rels;
	int result = -1;
	bool done = false;
	HWND list = nullptr;
};

static LRESULT CALLBACK alias_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	AliasDlg* d = (AliasDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg)
	{
	case WM_CREATE:
	{
		CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return 0;
	}
	case WM_COMMAND:
		if (d)
		{
			if (LOWORD(wp) == IDOK || (LOWORD(wp) == IDC_ALIAS_LIST && HIWORD(wp) == LBN_DBLCLK))
			{
				int sel = (int)SendMessageW(d->list, LB_GETCURSEL, 0, 0);
				if (sel >= 0)
				{
					d->result = sel;
					d->done = true;
				}
				return 0;
			}
			if (LOWORD(wp) == IDCANCEL)
			{
				d->done = true;
				return 0;
			}
		}
		break;
	case WM_CLOSE:
		if (d)
		{
			d->done = true;
		}
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void on_choose_alias()
{
	AliasDlg d;
	scan_aliases(g_alias_dir, std::wstring(), d.rels, 0);
	std::sort(d.rels.begin(), d.rels.end());
	if (d.rels.empty())
	{
		set_status(L"テキストのエイリアスが見つかりません。エイリアスを保存するフォルダ: " + g_alias_dir);
	}

	static bool registered = false;
	HINSTANCE inst = GetModuleHandleW(nullptr);
	if (!registered)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpszClassName = TELOP_ALIAS_CLASS;
		wc.lpfnWndProc = alias_dlg_proc;
		wc.hInstance = inst;
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClassExW(&wc);
		registered = true;
	}
	const int w = 520;
	const int h = 480;
	int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
	int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
	HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, TELOP_ALIAS_CLASS, L"テロップのエイリアスを選択", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, g_hwnd, nullptr, inst, &d);
	if (!dlg)
	{
		return;
	}
	HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	RECT rc;
	GetClientRect(dlg, &rc);
	int cw = rc.right;
	int ch = rc.bottom;
	HWND label = CreateWindowExW(0, L"STATIC", L"配置するテロップに使うエイリアスを選んでください (テキストのオブジェクトを含むものだけ表示しています)", WS_CHILD | WS_VISIBLE, 8, 6, cw - 16, 34, dlg, nullptr, inst, nullptr);
	d.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 8, 44, cw - 16, ch - 44 - 44, dlg, (HMENU)(INT_PTR)IDC_ALIAS_LIST, inst, nullptr);
	HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, cw - 8 - 90 - 8 - 90, ch - 36, 90, 28, dlg, (HMENU)(INT_PTR)IDOK, inst, nullptr);
	HWND cancel = CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, cw - 8 - 90, ch - 36, 90, 28, dlg, (HMENU)(INT_PTR)IDCANCEL, inst, nullptr);
	SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(d.list, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);

	SendMessageW(d.list, LB_ADDSTRING, 0, (LPARAM)alias_display_name(std::wstring()).c_str());
	int current = 0;
	for (size_t i = 0; i < d.rels.size(); i++)
	{
		SendMessageW(d.list, LB_ADDSTRING, 0, (LPARAM)alias_display_name(d.rels[i]).c_str());
		if (d.rels[i] == g_alias_rel)
		{
			current = (int)i + 1;
		}
	}
	SendMessageW(d.list, LB_SETCURSEL, (WPARAM)current, 0);

	EnableWindow(g_hwnd, FALSE);
	ShowWindow(dlg, SW_SHOW);
	SetFocus(d.list);
	MSG msg;
	while (!d.done)
	{
		int r = (int)GetMessageW(&msg, nullptr, 0, 0);
		if (r <= 0)
		{
			if (r == 0)
			{
				PostQuitMessage((int)msg.wParam);
			}
			break;
		}
		if (!IsDialogMessageW(dlg, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	EnableWindow(g_hwnd, TRUE);
	DestroyWindow(dlg);

	if (d.result >= 0)
	{
		g_alias_rel = (d.result == 0) ? std::wstring() : d.rels[(size_t)d.result - 1];
		save_alias_selection();
		update_alias_label();
		set_status(L"使用するエイリアスを「" + alias_display_name(g_alias_rel) + L"」にしました。次の「テロップ配置」から使われます");
	}
}

//---------------------------------------------------------------------
//	エイリアスの編集ダイアログ
//---------------------------------------------------------------------
struct AliasEditDlg
{
	std::vector<AliasParam> params;
	std::vector<std::wstring> orig;
	std::vector<std::wstring> values;
	bool done = false;
	bool ok = false;
	HWND list = nullptr;
	HWND value = nullptr;
	HWND color = nullptr;
};

static bool is_hex_color(const std::wstring& v)
{
	if (v.size() != 6)
	{
		return false;
	}
	for (wchar_t c : v)
	{
		if (!iswxdigit(c))
		{
			return false;
		}
	}
	return true;
}

static void ae_refresh_row(AliasEditDlg* d, int i)
{
	ListView_SetItemText(d->list, i, 2, (LPWSTR)d->values[(size_t)i].c_str());
	ListView_SetItemText(d->list, i, 3, (LPWSTR)((d->values[(size_t)i] != d->orig[(size_t)i]) ? L"変更あり" : L""));
}

static void ae_select(AliasEditDlg* d, int i)
{
	if (i < 0 || i >= (int)d->values.size())
	{
		return;
	}
	SetWindowTextW(d->value, d->values[(size_t)i].c_str());
	EnableWindow(d->color, is_hex_color(d->values[(size_t)i]) ? TRUE : FALSE);
}

static void ae_set_value(AliasEditDlg* d, int i, const std::wstring& v)
{
	d->values[(size_t)i] = v;
	ae_refresh_row(d, i);
	ae_select(d, i);
}

static LRESULT CALLBACK alias_edit_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	AliasEditDlg* d = (AliasEditDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg)
	{
	case WM_CREATE:
	{
		CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return 0;
	}
	case WM_NOTIFY:
	{
		NMHDR* nh = (NMHDR*)lp;
		if (d && nh->hwndFrom == d->list && nh->code == LVN_ITEMCHANGED)
		{
			NMLISTVIEW* nm = (NMLISTVIEW*)lp;
			if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
			{
				ae_select(d, nm->iItem);
			}
		}
		return 0;
	}
	case WM_COMMAND:
	{
		if (!d)
		{
			break;
		}
		int id = LOWORD(wp);
		int sel = ListView_GetNextItem(d->list, -1, LVNI_SELECTED);
		if (id == IDC_AE_APPLY && sel >= 0)
		{
			ae_set_value(d, sel, single_line(get_text(d->value)));
			return 0;
		}
		if (id == IDC_AE_REVERT && sel >= 0)
		{
			ae_set_value(d, sel, d->orig[(size_t)sel]);
			return 0;
		}
		if (id == IDC_AE_REVERT_ALL)
		{
			for (size_t i = 0; i < d->values.size(); i++)
			{
				d->values[i] = d->orig[i];
				ae_refresh_row(d, (int)i);
			}
			if (sel >= 0)
			{
				ae_select(d, sel);
			}
			return 0;
		}
		if (id == IDC_AE_COLOR && sel >= 0)
		{
			std::wstring cur = single_line(get_text(d->value));
			int r = 255;
			int g = 255;
			int b = 255;
			if (is_hex_color(cur))
			{
				r = (int)wcstoul(cur.substr(0, 2).c_str(), nullptr, 16);
				g = (int)wcstoul(cur.substr(2, 2).c_str(), nullptr, 16);
				b = (int)wcstoul(cur.substr(4, 2).c_str(), nullptr, 16);
			}
			static COLORREF custom[16] = {};
			CHOOSECOLORW cc = {};
			cc.lStructSize = sizeof(cc);
			cc.hwndOwner = hwnd;
			cc.rgbResult = RGB(r, g, b);
			cc.lpCustColors = custom;
			cc.Flags = CC_FULLOPEN | CC_RGBINIT;
			if (ChooseColorW(&cc))
			{
				char hex[16];
				snprintf(hex, sizeof(hex), "%02x%02x%02x", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
				ae_set_value(d, sel, ascii_to_w(hex));
			}
			return 0;
		}
		if (id == IDOK)
		{
			d->ok = true;
			d->done = true;
			return 0;
		}
		if (id == IDCANCEL)
		{
			d->done = true;
			return 0;
		}
		break;
	}
	case WM_CLOSE:
		if (d)
		{
			d->done = true;
		}
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void on_edit_alias()
{
	AliasEditDlg d;
	parse_alias_params(base_template(), d.params);
	if (d.params.empty())
	{
		set_status(L"このエイリアスには、編集できる項目がありません");
		return;
	}
	std::wstring key = alias_edit_key();
	for (const AliasParam& p : d.params)
	{
		d.orig.push_back(p.value);
		std::wstring v = p.value;
		for (const AliasEdit& e : g_alias_edits)
		{
			if (e.rel == key && e.section == p.section && e.key == p.key)
			{
				v = e.value;
			}
		}
		d.values.push_back(v);
	}

	static bool registered = false;
	HINSTANCE inst = GetModuleHandleW(nullptr);
	if (!registered)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpszClassName = TELOP_ALIAS_EDIT_CLASS;
		wc.lpfnWndProc = alias_edit_dlg_proc;
		wc.hInstance = inst;
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClassExW(&wc);
		registered = true;
	}
	const int w = 680;
	const int h = 560;
	int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
	int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
	HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, TELOP_ALIAS_EDIT_CLASS, (L"エイリアスの編集: " + alias_display_name(g_alias_rel)).c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, g_hwnd, nullptr, inst, &d);
	if (!dlg)
	{
		return;
	}
	HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	RECT rc;
	GetClientRect(dlg, &rc);
	int cw = rc.right;
	int ch = rc.bottom;
	auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int cx, int cy, int cwid, int chei, DWORD ex = 0)
		{
			HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, cx, cy, cwid, chei, dlg, (HMENU)(INT_PTR)id, inst, nullptr);
			SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
			return c;
		};
	child(L"STATIC", L"項目を選んで、下の欄で値を変えて「値を反映」を押します (元のエイリアスのファイルは変更されません)", 0, 0, 8, 6, cw - 16, 20);
	d.list = child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_AE_LIST, 8, 30, cw - 16, ch - 30 - 130, WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(d.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	const wchar_t* names[] = { L"効果", L"項目", L"値", L"変更" };
	const int widths[] = { 140, 130, 280, 80 };
	for (int i = 0; i < 4; i++)
	{
		LVCOLUMNW col = {};
		col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		col.pszText = (LPWSTR)names[i];
		col.cx = widths[i];
		col.iSubItem = i;
		ListView_InsertColumn(d.list, i, &col);
	}
	for (size_t i = 0; i < d.params.size(); i++)
	{
		std::wstring eff = d.params[i].effect.empty() ? d.params[i].section : d.params[i].effect;
		LVITEMW it = {};
		it.mask = LVIF_TEXT;
		it.iItem = (int)i;
		it.pszText = (LPWSTR)eff.c_str();
		ListView_InsertItem(d.list, &it);
		ListView_SetItemText(d.list, (int)i, 1, (LPWSTR)d.params[i].key.c_str());
		ae_refresh_row(&d, (int)i);
	}
	int by = ch - 124;
	child(L"STATIC", L"値:", 0, 0, 8, by + 4, 30, 22);
	d.value = child(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_AE_VALUE, 40, by, cw - 40 - 8 - 110, 26, WS_EX_CLIENTEDGE);
	d.color = child(WC_BUTTONW, L"色を選択...", BS_PUSHBUTTON | WS_TABSTOP, IDC_AE_COLOR, cw - 8 - 100, by, 100, 26);
	by += 34;
	child(WC_BUTTONW, L"値を反映", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_AE_APPLY, 8, by, 110, 28);
	child(WC_BUTTONW, L"この項目を元に戻す", BS_PUSHBUTTON | WS_TABSTOP, IDC_AE_REVERT, 126, by, 150, 28);
	child(WC_BUTTONW, L"すべて元に戻す", BS_PUSHBUTTON | WS_TABSTOP, IDC_AE_REVERT_ALL, 284, by, 130, 28);
	by += 40;
	child(WC_BUTTONW, L"OK", BS_PUSHBUTTON | WS_TABSTOP, IDOK, cw - 8 - 100 - 8 - 100, by, 100, 28);
	child(WC_BUTTONW, L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, cw - 8 - 100, by, 100, 28);
	ListView_SetItemState(d.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	ae_select(&d, 0);

	EnableWindow(g_hwnd, FALSE);
	ShowWindow(dlg, SW_SHOW);
	SetFocus(d.value);
	MSG msg;
	while (!d.done)
	{
		int r = (int)GetMessageW(&msg, nullptr, 0, 0);
		if (r <= 0)
		{
			if (r == 0)
			{
				PostQuitMessage((int)msg.wParam);
			}
			break;
		}
		if (!IsDialogMessageW(dlg, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	EnableWindow(g_hwnd, TRUE);
	DestroyWindow(dlg);

	if (d.ok)
	{
		g_alias_edits.erase(std::remove_if(g_alias_edits.begin(), g_alias_edits.end(), [&](const AliasEdit& e)
			{
				return e.rel == key;
			}), g_alias_edits.end());
		int changed = 0;
		for (size_t i = 0; i < d.params.size(); i++)
		{
			if (d.values[i] != d.orig[i])
			{
				AliasEdit e;
				e.rel = key;
				e.section = d.params[i].section;
				e.key = d.params[i].key;
				e.value = d.values[i];
				g_alias_edits.push_back(e);
				changed++;
			}
		}
		save_alias_edits();
		update_alias_label();
		set_status(L"エイリアスの編集を保存しました (" + std::to_wstring(changed) + L"項目)。次の「テロップ配置」から反映されます");
	}
}

static std::wstring find_model(std::wstring& vad)
{
	std::wstring dir = g_dll_dir + L"\\models";
	std::wstring best;
	ULONGLONG best_size = 0;
	vad.clear();
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*.bin").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		return best;
	}
	do
	{
		std::wstring name = fd.cFileName;
		std::wstring lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
		if (lower.find(L"silero") != std::wstring::npos || lower.find(L"vad") != std::wstring::npos)
		{
			vad = dir + L"\\" + name;
			continue;
		}
		ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
		if (size > best_size)
		{
			best_size = size;
			best = dir + L"\\" + name;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return best;
}

static void on_run()
{
	if (g_running)
	{
		return;
	}
	std::wstring vad;
	std::wstring model = find_model(vad);
	if (model.empty())
	{
		set_status(L"モデルがありません。次のフォルダに ggml-large-v3-q5_0.bin などを置いてください: " + g_dll_dir + L"\\models");
		return;
	}
	EDIT_INFO info = {};
	g_edit->get_edit_info(&info, sizeof(info));

	// 選択中のレイヤー上のオブジェクトだけを対象にする (他のレイヤーの音声は文字起こししない)
	struct TargetCtx
	{
		int layer = 0;
		int rs = -1;
		int re = -1;
		std::vector<Target> list;
	};
	TargetCtx tc;
	tc.layer = info.layer;
	tc.rs = info.select_range_start;
	tc.re = info.select_range_end;
	bool called = g_edit->call_read_section_param(&tc, [](void* param, EDIT_SECTION* e)
		{
			TargetCtx* c = (TargetCtx*)param;
			// ※読み取り専用のセクションでは e->info は使えないため、レイヤーは事前に取得した値を使う
			int frame = 0;
			while (frame < 100000000)
			{
				OBJECT_HANDLE o = e->find_object(c->layer, frame);
				if (!o)
				{
					break;
				}
				OBJECT_LAYER_FRAME lf = e->get_object_layer_frame(o);
				if (lf.end + 1 <= frame)
				{
					break;
				}
				frame = lf.end + 1;
				int s = lf.start;
				int en = lf.end + 1;
				if (c->rs >= 0 && c->re >= 0)
				{
					s = std::max(s, c->rs);
					en = std::min(en, c->re + 1);
				}
				if (en > s)
				{
					Target t;
					t.layer = c->layer;
					t.obj_start = lf.start;
					t.start = s;
					t.end = en;
					c->list.push_back(t);
				}
			}
		});
	if (!called)
	{
		set_status(L"実行できませんでした (出力中などは実行できません)");
		return;
	}
	if (tc.list.empty())
	{
		set_status(L"レイヤー" + std::to_wstring(tc.layer + 1) + L"に対象のオブジェクトがありません。文字起こししたいオブジェクトのあるレイヤーを選択してください");
		return;
	}
	RunParams p;
	p.targets = tc.list;
	p.layer = tc.layer;
	p.start_frame = INT_MAX;
	p.end_frame = 0;
	for (const Target& t : p.targets)
	{
		p.start_frame = std::min(p.start_frame, t.start);
		p.end_frame = std::max(p.end_frame, t.end);
	}
	p.rate = info.rate;
	p.scale = info.scale;
	p.sample_rate = (info.sample_rate > 0) ? info.sample_rate : 48000;
	p.model_path = model;
	p.st = load_settings();
	p.st.separate = (IsDlgButtonChecked(g_hwnd, IDC_SEPARATE) == BST_CHECKED);
	MAX_CHARS = (size_t)p.st.max_chars;
	p.vad_path = p.st.use_vad ? vad : std::wstring();
	p.dict = g_corrections;
	p.ignore = load_ignore();
	p.debug_wav = g_data_dir + L"\\last_capture.wav";
	p.debug_txt = g_data_dir + L"\\last_result.txt";
	p.prompt = build_prompt(g_corrections);
	p.hwnd = g_hwnd;

	join_worker();
	g_cancel = false;
	g_running = true;
	EnableWindow(GetDlgItem(g_hwnd, IDC_RUN), FALSE);
	EnableWindow(GetDlgItem(g_hwnd, IDC_CANCEL), TRUE);
	set_status(L"レイヤー" + std::to_wstring(p.layer + 1) + L"のオブジェクトの音声を取得中...");
	g_worker = CreateThread(nullptr, 64 * 1024 * 1024, worker_thread_proc, new RunParams(p), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (!g_worker)
	{
		g_running = false;
		EnableWindow(GetDlgItem(g_hwnd, IDC_RUN), TRUE);
		EnableWindow(GetDlgItem(g_hwnd, IDC_CANCEL), FALSE);
		set_status(L"処理を開始できませんでした");
	}
}

static void on_cancel()
{
	if (g_running)
	{
		g_cancel = true;
		set_status(L"中止しています...");
	}
}

static void on_place()
{
	if (g_rows.empty())
	{
		set_status(L"配置する字幕がありません");
		return;
	}
	struct Ctx
	{
		std::vector<Row> rows;
		std::vector<Placed> placed;
		std::string tmpl;
		int ok = 0;
		int ng = 0;
		int layer = 0;
	};
	Ctx ctx;
	ctx.rows = g_rows;
	ctx.tmpl = current_template();

	bool called = g_edit->call_edit_section_param(&ctx, [](void* param, EDIT_SECTION* edit)
		{
			Ctx* c = (Ctx*)param;
			// 配置先は、選択中のレイヤー
			int layer = edit->info->layer;
			c->layer = layer;
			for (const Row& r : c->rows)
			{
				if (!r.check)
				{
					continue;
				}
				std::string alias = build_alias(c->tmpl, r.text);
				int length = std::max(1, r.end - r.start);
				OBJECT_HANDLE obj = edit->create_object_from_alias(alias.c_str(), layer, r.start, length);
				if (obj)
				{
					Placed p;
					p.id = edit->get_object_id(obj);
					p.layer = layer;
					p.start = r.start;
					p.raw = r.raw;
					p.text = r.text;
					c->placed.push_back(p);
					c->ok++;
				}
				else
				{
					c->ng++;
				}
			}
		});
	if (!called)
	{
		set_status(L"配置できませんでした (出力中などは実行できません)");
		return;
	}
	for (const Placed& p : ctx.placed)
	{
		g_placed.push_back(p);
	}
	save_placed();
	std::wstring m = L"レイヤー" + std::to_wstring(ctx.layer + 1) + L"に" + std::to_wstring(ctx.ok) + L"件を配置しました";
	if (ctx.ng > 0)
	{
		m += L" (" + std::to_wstring(ctx.ng) + L"件は既存のオブジェクトと重なるため配置できませんでした)";
	}
	set_status(m);
}

static void add_candidate(std::vector<std::pair<std::wstring, std::wstring>>& list, const std::wstring& raw, const std::wstring& fixed)
{
	std::wstring from;
	std::wstring to;
	if (!make_pair_from_diff(raw, fixed, from, to))
	{
		return;
	}
	for (const auto& c : list)
	{
		if (c.first == from && c.second == to)
		{
			return;
		}
	}
	list.push_back(std::make_pair(from, to));
}

static void on_learn()
{
	std::vector<std::pair<std::wstring, std::wstring>> cands;

	// 一覧上で修正した内容
	for (const Row& r : g_rows)
	{
		add_candidate(cands, r.raw, r.text);
	}

	// タイムライン上で修正した内容
	struct Ctx
	{
		std::vector<Placed>* placed = nullptr;
		std::vector<std::pair<std::wstring, std::wstring>>* cands = nullptr;
		std::vector<size_t> learned;
	};
	Ctx ctx;
	ctx.placed = &g_placed;
	ctx.cands = &cands;
	g_edit->call_read_section_param(&ctx, [](void* param, EDIT_SECTION* edit)
		{
			Ctx* c = (Ctx*)param;
			for (size_t i = 0; i < c->placed->size(); i++)
			{
				const Placed& pl = (*c->placed)[i];
				OBJECT_HANDLE obj = edit->find_object(pl.layer, pl.start);
				if (!obj || edit->get_object_id(obj) != pl.id)
				{
					continue;
				}
				LPCSTR v = edit->get_object_item_value(obj, L"テキスト", L"テキスト");
				if (!v)
				{
					continue;
				}
				std::wstring cur = from_utf8(v);
				if (cur != pl.text)
				{
					add_candidate(*c->cands, pl.raw, cur);
					c->learned.push_back(i);
				}
			}
		});

	if (cands.empty())
	{
		set_status(L"学習する修正はありませんでした");
		return;
	}

	std::wstring msg = std::to_wstring(cands.size()) + L"件の修正を辞書に登録します。よろしいですか？\n\n";
	for (size_t i = 0; i < cands.size() && i < 12; i++)
	{
		msg += L"「" + cands[i].first + L"」→「" + cands[i].second + L"」\n";
	}
	if (cands.size() > 12)
	{
		msg += L"…他" + std::to_wstring(cands.size() - 12) + L"件\n";
	}
	if (MessageBoxW(g_hwnd, msg.c_str(), L"修正を学習", MB_YESNO | MB_ICONQUESTION) != IDYES)
	{
		set_status(L"学習をキャンセルしました");
		return;
	}

	for (const auto& c : cands)
	{
		bool done = false;
		for (size_t i = 0; i < g_corrections.size(); i++)
		{
			Correction& r = g_corrections[i];
			if (r.from == c.first)
			{
				r.to = c.second;
				r.count++;
				done = true;
				break;
			}
			if (r.from == c.second && r.to == c.first)
			{
				// 逆方向の修正は、既存のルールを取り消す扱いにする
				g_corrections.erase(g_corrections.begin() + i);
				done = true;
				break;
			}
		}
		if (!done)
		{
			Correction r;
			r.from = c.first;
			r.to = c.second;
			r.count = 1;
			g_corrections.push_back(r);
		}
	}
	save_corrections();

	for (Row& r : g_rows)
	{
		r.raw = r.text;
	}
	for (size_t k = ctx.learned.size(); k > 0; k--)
	{
		g_placed.erase(g_placed.begin() + ctx.learned[k - 1]);
	}
	save_placed();
	set_status(std::to_wstring(cands.size()) + L"件を辞書に登録しました (合計 " + std::to_wstring(g_corrections.size()) + L"件)");
}

static void on_apply()
{
	int i = selected_index();
	if (i < 0 || i >= (int)g_rows.size())
	{
		return;
	}
	double s = 0;
	double e = 0;
	if (!parse_time(get_text(g_edit_start), s) || !parse_time(get_text(g_edit_end), e))
	{
		set_status(L"時間は「1:23.4」または「83.4」の形式で入力してください");
		return;
	}
	int fs = sec_to_frame(s);
	int fe = sec_to_frame(e);
	if (fe <= fs)
	{
		set_status(L"終了は開始より後にしてください");
		return;
	}
	g_rows[i].start = fs;
	g_rows[i].end = fe;
	g_rows[i].text = single_line(get_text(g_edit_text));
	refresh_list(i);
}

static void on_split()
{
	int i = selected_index();
	if (i < 0 || i >= (int)g_rows.size())
	{
		return;
	}
	DWORD a = 0;
	DWORD b = 0;
	SendMessageW(g_edit_text, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
	std::wstring t = single_line(get_text(g_edit_text));
	if (a == 0 || a >= t.size())
	{
		set_status(L"分割したい位置にカーソルを置いてください");
		return;
	}
	Row r = g_rows[i];
	if (r.end - r.start < 2)
	{
		set_status(L"短すぎるため分割できません");
		return;
	}
	int mid = r.start + (int)((int64_t)(r.end - r.start) * a / t.size());
	mid = std::min(std::max(mid, r.start + 1), r.end - 1);
	Row second = r;
	r.text = t.substr(0, a);
	r.end = mid;
	second.text = t.substr(a);
	second.raw = second.text;
	r.raw = r.text;
	second.start = mid;
	g_rows[i] = r;
	g_rows.insert(g_rows.begin() + i + 1, second);
	refresh_list(i);
	load_edit_boxes(i);
}

static void on_merge()
{
	int i = selected_index();
	if (i < 0 || i + 1 >= (int)g_rows.size())
	{
		return;
	}
	g_rows[i].text += g_rows[i + 1].text;
	g_rows[i].raw += g_rows[i + 1].raw;
	g_rows[i].end = g_rows[i + 1].end;
	g_rows.erase(g_rows.begin() + i + 1);
	refresh_list(i);
	load_edit_boxes(i);
}

static void on_delete()
{
	int i = selected_index();
	if (i < 0 || i >= (int)g_rows.size())
	{
		return;
	}
	g_rows.erase(g_rows.begin() + i);
	refresh_list(std::min(i, (int)g_rows.size() - 1));
}

//---------------------------------------------------------------------
//	ウィンドウ
//---------------------------------------------------------------------
static void do_layout(int w, int h)
{
	int ih = 26;
	if (g_config)
	{
		int v = g_config->get_layout_size(g_config, "SettingItemHeight");
		if (v > 0)
		{
			ih = v;
		}
	}
	int m = 6;
	int x = m;
	int y = m;
	struct B
	{
		int id;
		int width;
	};
	const B top[] = { { IDC_RUN, 90 }, { IDC_CANCEL, 60 }, { IDC_PLACE, 90 }, { IDC_LEARN, 100 }, { IDC_FOLDER, 130 }, { IDC_SEPARATE, 100 } };
	for (const B& b : top)
	{
		MoveWindow(GetDlgItem(g_hwnd, b.id), x, y, b.width, ih, TRUE);
		x += b.width + m;
	}
	y += ih + m;
	MoveWindow(GetDlgItem(g_hwnd, IDC_ALIAS), m, y, 140, ih, TRUE);
	MoveWindow(GetDlgItem(g_hwnd, IDC_ALIAS_EDIT), m + 140 + m, y, 140, ih, TRUE);
	MoveWindow(g_alias_label, m + (140 + m) * 2, y + 4, std::max(40, w - (140 + m) * 2 - m * 2), ih, TRUE);
	y += ih + m;
	MoveWindow(g_status, m, y, w - m * 2, ih * 3, TRUE);
	y += ih * 3 + m;

	int bottom_h = ih * 2 + m * 3;
	int list_h = std::max(60, h - y - bottom_h);
	MoveWindow(g_list, m, y, w - m * 2, list_h, TRUE);
	y += list_h + m;

	// 下段1: 開始・終了・ボタン
	MoveWindow(g_edit_start, m, y, 90, ih, TRUE);
	MoveWindow(g_edit_end, m + 96, y, 90, ih, TRUE);
	x = m + 96 * 2 + 6;
	const B btns[] = { { IDC_APPLY, 60 }, { IDC_SPLIT, 60 }, { IDC_MERGE, 60 }, { IDC_DELETE, 60 } };
	for (const B& b : btns)
	{
		MoveWindow(GetDlgItem(g_hwnd, b.id), x, y, b.width, ih, TRUE);
		x += b.width + m;
	}
	y += ih + m;
	// 下段2: テキスト
	MoveWindow(g_edit_text, m, y, w - m * 2, ih, TRUE);
}

static WNDPROC g_edit_old_proc = nullptr;

// 入力欄で Enter を押したら「反映」と同じ動きにする
static LRESULT CALLBACK edit_subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_KEYDOWN && wp == VK_RETURN)
	{
		on_apply();
		return 0;
	}
	if (msg == WM_CHAR && wp == VK_RETURN)
	{
		return 0;
	}
	return CallWindowProcW(g_edit_old_proc, h, msg, wp, lp);
}

static HWND make_child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0)
{
	HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
	SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
	return h;
}

static void create_controls()
{
	make_child(WC_BUTTONW, L"文字起こし", BS_PUSHBUTTON, IDC_RUN);
	make_child(WC_BUTTONW, L"中止", BS_PUSHBUTTON, IDC_CANCEL);
	EnableWindow(GetDlgItem(g_hwnd, IDC_CANCEL), FALSE);
	make_child(WC_BUTTONW, L"テロップ配置", BS_PUSHBUTTON, IDC_PLACE);
	make_child(WC_BUTTONW, L"修正を学習", BS_PUSHBUTTON, IDC_LEARN);
	make_child(WC_BUTTONW, L"データフォルダを開く", BS_PUSHBUTTON, IDC_FOLDER);
	make_child(WC_BUTTONW, L"声を抽出", BS_AUTOCHECKBOX, IDC_SEPARATE);
	make_child(WC_BUTTONW, L"エイリアスを選択", BS_PUSHBUTTON, IDC_ALIAS);
	make_child(WC_BUTTONW, L"エイリアスを編集", BS_PUSHBUTTON, IDC_ALIAS_EDIT);
	g_alias_label = make_child(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, IDC_ALIAS_LABEL);
	CheckDlgButton(g_hwnd, IDC_SEPARATE, load_settings().separate ? BST_CHECKED : BST_UNCHECKED);
	g_status = make_child(WC_STATICW, L"文字起こししたいオブジェクトのあるレイヤーを選択して「文字起こし」を押してください (範囲選択があれば、その範囲だけ)", SS_LEFT, IDC_STATUS);

	g_list = make_child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, IDC_LIST);
	ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
	const wchar_t* names[] = { L"#", L"開始", L"終了", L"テキスト" };
	const int widths[] = { 60, 80, 80, 600 };
	for (int i = 0; i < 4; i++)
	{
		LVCOLUMNW col = {};
		col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		col.pszText = (LPWSTR)names[i];
		col.cx = widths[i];
		col.iSubItem = i;
		ListView_InsertColumn(g_list, i, &col);
	}

	g_edit_start = make_child(WC_EDITW, L"", ES_AUTOHSCROLL, IDC_EDIT_START, WS_EX_CLIENTEDGE);
	g_edit_end = make_child(WC_EDITW, L"", ES_AUTOHSCROLL, IDC_EDIT_END, WS_EX_CLIENTEDGE);
	g_edit_text = make_child(WC_EDITW, L"", ES_AUTOHSCROLL, IDC_EDIT_TEXT, WS_EX_CLIENTEDGE);
	HWND edits[3] = { g_edit_start, g_edit_end, g_edit_text };
	for (HWND e : edits)
	{
		WNDPROC old = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc);
		if (!g_edit_old_proc)
		{
			g_edit_old_proc = old;
		}
	}
	make_child(WC_BUTTONW, L"反映", BS_PUSHBUTTON, IDC_APPLY);
	make_child(WC_BUTTONW, L"分割", BS_PUSHBUTTON, IDC_SPLIT);
	make_child(WC_BUTTONW, L"結合", BS_PUSHBUTTON, IDC_MERGE);
	make_child(WC_BUTTONW, L"削除", BS_PUSHBUTTON, IDC_DELETE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_SIZE:
		do_layout(LOWORD(lparam), HIWORD(lparam));
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wparam))
		{
		case IDC_RUN:
			on_run();
			break;
		case IDC_CANCEL:
			on_cancel();
			break;
		case IDC_PLACE:
			on_place();
			break;
		case IDC_ALIAS:
			on_choose_alias();
			break;
		case IDC_ALIAS_EDIT:
			on_edit_alias();
			break;
		case IDC_LEARN:
			on_learn();
			break;
		case IDC_FOLDER:
			make_dirs(g_data_dir);
			ShellExecuteW(nullptr, L"open", g_data_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			break;
		case IDC_APPLY:
			on_apply();
			break;
		case IDC_SPLIT:
			on_split();
			break;
		case IDC_MERGE:
			on_merge();
			break;
		case IDC_DELETE:
			on_delete();
			break;
		}
		// ボタンを押した後だけフォーカスを外す (入力欄の通知でフォーカスを外すと、文字を入力できなくなる)
		if (HIWORD(wparam) == BN_CLICKED && lparam != 0)
		{
			char cls[16] = "";
			GetClassNameA((HWND)lparam, cls, sizeof(cls));
			if (_stricmp(cls, "Button") == 0)
			{
				SetFocus(nullptr);
			}
		}
		return 0;

	case WM_NOTIFY:
	{
		NMHDR* nh = (NMHDR*)lparam;
		if (nh->hwndFrom == g_list && nh->code == NM_DBLCLK)
		{
			SetFocus(g_edit_text);
			SendMessageW(g_edit_text, EM_SETSEL, 0, -1);
		}
		if (nh->hwndFrom == g_list && nh->code == LVN_ITEMCHANGED && !g_updating)
		{
			NMLISTVIEW* nm = (NMLISTVIEW*)lparam;
			if ((nm->uChanged & LVIF_STATE) && nm->iItem >= 0 && nm->iItem < (int)g_rows.size())
			{
				if ((nm->uOldState ^ nm->uNewState) & LVIS_STATEIMAGEMASK)
				{
					g_rows[nm->iItem].check = ListView_GetCheckState(g_list, nm->iItem) ? true : false;
				}
				if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
				{
					load_edit_boxes(nm->iItem);
					jump_to(g_rows[nm->iItem].start);
				}
			}
		}
		return 0;
	}

	case WM_APP_PROGRESS:
	{
		std::wstring m = L"処理中... " + std::to_wstring((int)wparam) + L"%";
		set_status(m);
		return 0;
	}

	case WM_APP_STATUS:
	{
		std::wstring* m = (std::wstring*)lparam;
		set_status(*m);
		delete m;
		return 0;
	}

	case WM_APP_DONE:
	{
		Result* r = (Result*)lparam;
		join_worker();
		g_running = false;
		EnableWindow(GetDlgItem(g_hwnd, IDC_RUN), TRUE);
		EnableWindow(GetDlgItem(g_hwnd, IDC_CANCEL), FALSE);
		if (r)
		{
			g_tb = r->tb;
			g_rows = r->rows;
			std::wstring r_note = r->note;
			delete r;
			refresh_list(g_rows.empty() ? -1 : 0);
			std::wstring m;
			if (g_rows.empty())
			{
				m = L"字幕は認識されませんでした (話し声が無い可能性があります)";
			}
			else
			{
				m = std::to_wstring(g_rows.size()) + L"件の字幕を認識しました。修正してから、配置したいレイヤーを選択して「テロップ配置」を押してください";
			}
			if (!r_note.empty())
			{
				m += L"  [" + r_note + L"]";
			}
			set_status(m);
		}
		return 0;
	}
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

//---------------------------------------------------------------------
//	汎用プラグイン構造体定義
//---------------------------------------------------------------------
COMMON_PLUGIN_TABLE common_plugin_table = {
	L"Telop Generator",
	L"Telop Generator version 0.01 (whisper.cpp built-in)",
};

EXTERN_C __declspec(dllexport) DWORD RequiredVersion()
{
	return 2003300;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle)
{
	g_logger = handle;
}

EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle)
{
	g_config = handle;
	std::wstring base = handle->app_data_path;
	while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
	{
		base.pop_back();
	}
	// アプリケーションデータフォルダ(Plugin や Alias と同じ階層)の直下に専用フォルダを作る
	g_data_dir = base + L"\\" TELOP_DATA_FOLDER;
	g_alias_dir = base + L"\\Alias";
	// 以前の版で作っていた場所 (data\data\TelopGenerator) にデータがあれば移動する
	std::wstring old_dir = base + L"\\data\\" TELOP_DATA_FOLDER;
	DWORD a_old = GetFileAttributesW(old_dir.c_str());
	if (a_old != INVALID_FILE_ATTRIBUTES && (a_old & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesW(g_data_dir.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		MoveFileW(old_dir.c_str(), g_data_dir.c_str());
	}
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version)
{
	g_dll_dir = get_dll_dir();
	return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin()
{
	g_cancel = true;
	join_worker();
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void)
{
	return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host)
{
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);

	if (g_data_dir.empty())
	{
		g_data_dir = get_dll_dir() + L"\\data";
	}
	make_dirs(g_data_dir);
	if (g_dll_dir.empty())
	{
		g_dll_dir = get_dll_dir();
	}
	make_dirs(g_dll_dir + L"\\models");
	load_corrections();
	load_placed();

	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(wcex);
	wcex.lpszClassName = TELOP_CLASS_NAME;
	wcex.lpfnWndProc = wnd_proc;
	wcex.hInstance = GetModuleHandleW(nullptr);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	if (!RegisterClassExW(&wcex))
	{
		return;
	}
	g_hwnd = CreateWindowExW(0, TELOP_CLASS_NAME, TELOP_WINDOW_NAME, WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!g_hwnd)
	{
		return;
	}
	create_controls();
	load_alias_edits();
	load_alias_selection();
	g_edit = host->create_edit_handle();
	host->register_window_client(TELOP_WINDOW_NAME, g_hwnd);
}