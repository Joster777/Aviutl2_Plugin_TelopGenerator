//----------------------------------------------------------------------------------
//	声の抽出 (BGMや効果音から声だけを取り出す)
//	UVR の MDX-Net 形式のONNXモデル(UVR-MDX-NET-Voc_FT など)を ONNX Runtime で実行します
//	ONNX Runtime は onnxruntime.dll を実行時に読み込むため、ビルド時にライブラリは不要です
//----------------------------------------------------------------------------------
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "onnxruntime_c_api.h"
#include "pocketfft_hdronly.h"

namespace voicesep
{
	//---------------------------------------------------------------------
	//	リサンプラ (窓付きsinc / 多相フィルタ)
	//---------------------------------------------------------------------
	inline int64_t gcd64(int64_t a, int64_t b)
	{
		while (b != 0)
		{
			int64_t t = a % b;
			a = b;
			b = t;
		}
		return a;
	}

	inline std::vector<float> resample(const std::vector<float>& in, int sr_in, int sr_out)
	{
		if (sr_in == sr_out || in.empty())
		{
			return in;
		}
		const double pi = 3.14159265358979323846;
		int64_t g = gcd64(sr_in, sr_out);
		int64_t phases = sr_out / g;
		double cut = std::min(1.0, (double)sr_out / (double)sr_in) * 0.94;	// 入力側のナイキストに対する比
		int half = (int)std::ceil(16.0 / cut);									// 片側のタップ数
		int taps = half * 2;

		// 位相ごとのフィルタ係数
		std::vector<float> table((size_t)phases * taps);
		for (int64_t ph = 0; ph < phases; ph++)
		{
			double frac = (double)ph / (double)phases;		// 出力位置の小数部 (入力サンプル単位)
			double sum = 0;
			for (int j = 0; j < taps; j++)
			{
				double d = (double)(j - half + 1) - frac;		// 入力サンプルまでの距離
				double x = d * cut;
				double s = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(pi * x) / (pi * x);
				double w = 0.5 * (1.0 + std::cos(pi * d / (double)half));
				double v = (std::fabs(d) < half) ? s * w : 0.0;
				table[(size_t)ph * taps + j] = (float)v;
				sum += v;
			}
			for (int j = 0; j < taps; j++)
			{
				table[(size_t)ph * taps + j] = (float)(table[(size_t)ph * taps + j] / sum);
			}
		}

		int64_t n_in = (int64_t)in.size();
		int64_t n_out = n_in * sr_out / sr_in;
		std::vector<float> out((size_t)n_out);
		for (int64_t i = 0; i < n_out; i++)
		{
			int64_t num = i * sr_in;
			int64_t base = num / sr_out;
			int64_t ph = (num % sr_out) / g;
			const float* f = &table[(size_t)ph * taps];
			int64_t start = base - half + 1;
			double acc = 0;
			if (start >= 0 && start + taps <= n_in)
			{
				for (int j = 0; j < taps; j++)
				{
					acc += (double)in[(size_t)(start + j)] * f[j];
				}
			}
			else
			{
				for (int j = 0; j < taps; j++)
				{
					int64_t idx = start + j;
					if (idx >= 0 && idx < n_in)
					{
						acc += (double)in[(size_t)idx] * f[j];
					}
				}
			}
			out[(size_t)i] = (float)acc;
		}
		return out;
	}

	//---------------------------------------------------------------------
	//	ONNX Runtime (C API を実行時に読み込む)
	//---------------------------------------------------------------------
	class OrtRunner
	{
	public:
		OrtRunner() {}
		~OrtRunner()
		{
			close();
		}

		// runtime_dll: onnxruntime.dll のパス, use_gpu: DirectMLを使う, device_id: GPUの番号
		bool open(const std::wstring& runtime_dll, const std::wstring& model_path, bool use_gpu, int device_id, int cpu_threads, std::string& err)
		{
			close();
#ifdef _WIN32
			lib_ = (void*)LoadLibraryExW(runtime_dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (!lib_)
			{
				err = "onnxruntime.dll を読み込めません";
				return false;
			}
			void* sym = (void*)GetProcAddress((HMODULE)lib_, "OrtGetApiBase");
#else
			std::string p(runtime_dll.begin(), runtime_dll.end());
			lib_ = dlopen(p.c_str(), RTLD_NOW);
			if (!lib_)
			{
				err = "onnxruntime を読み込めません";
				return false;
			}
			void* sym = dlsym(lib_, "OrtGetApiBase");
#endif
			if (!sym)
			{
				err = "OrtGetApiBase が見つかりません";
				return false;
			}
			const OrtApiBase* base = ((const OrtApiBase * (*)(void))sym)();
			api_ = nullptr;
			for (int v = ORT_API_VERSION; v >= 17 && !api_; v--)
			{
				api_ = base->GetApi((uint32_t)v);
			}
			if (!api_)
			{
				err = "onnxruntime のバージョンが古すぎます (1.17以降が必要です)";
				return false;
			}
			if (!check(api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "TelopGenerator", &env_), err))
			{
				return false;
			}
			if (!check(api_->CreateSessionOptions(&opts_), err))
			{
				return false;
			}
			ignore(api_->SetSessionGraphOptimizationLevel(opts_, ORT_ENABLE_ALL));
			if (cpu_threads > 0)
			{
				ignore(api_->SetIntraOpNumThreads(opts_, cpu_threads));
			}
			using_gpu_ = false;
			if (use_gpu)
			{
				ignore(api_->DisableMemPattern(opts_));
				ignore(api_->SetSessionExecutionMode(opts_, ORT_SEQUENTIAL));
				std::string id = std::to_string(device_id);
				const char* keys[] = { "device_id" };
				const char* vals[] = { id.c_str() };
				OrtStatus* st = api_->SessionOptionsAppendExecutionProvider(opts_, "DML", keys, vals, 1);
				if (st)
				{
					api_->ReleaseStatus(st);		// GPUが使えない場合はCPUで動かす
				}
				else
				{
					using_gpu_ = true;
				}
			}
#ifdef _WIN32
			OrtStatus* ss = api_->CreateSession(env_, model_path.c_str(), opts_, &session_);
#else
			std::string mp(model_path.begin(), model_path.end());
			OrtStatus* ss = api_->CreateSession(env_, mp.c_str(), opts_, &session_);
#endif
			if (ss && using_gpu_)
			{
				// GPUでセッションを作れなければ、CPUで作り直す
				api_->ReleaseStatus(ss);
				api_->ReleaseSessionOptions(opts_);
				opts_ = nullptr;
				if (!check(api_->CreateSessionOptions(&opts_), err))
				{
					return false;
				}
				ignore(api_->SetSessionGraphOptimizationLevel(opts_, ORT_ENABLE_ALL));
				if (cpu_threads > 0)
				{
					ignore(api_->SetIntraOpNumThreads(opts_, cpu_threads));
				}
				using_gpu_ = false;
#ifdef _WIN32
				ss = api_->CreateSession(env_, model_path.c_str(), opts_, &session_);
#else
				ss = api_->CreateSession(env_, mp.c_str(), opts_, &session_);
#endif
			}
			if (!check(ss, err))
			{
				return false;
			}
			if (!check(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_), err))
			{
				return false;
			}
			return true;
		}

		bool using_gpu() const
		{
			return using_gpu_;
		}

		// 入力 [1,4,dim_f,dim_t] → 出力 同じ形
		bool run(const float* in, float* out, int dim_f, int dim_t, std::string& err)
		{
			int64_t shape[4] = { 1, 4, dim_f, dim_t };
			size_t count = (size_t)4 * dim_f * dim_t;
			OrtValue* in_val = nullptr;
			OrtValue* out_val = nullptr;
			if (!check(api_->CreateTensorWithDataAsOrtValue(mem_, (void*)in, count * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val), err))
			{
				return false;
			}
			const char* in_names[] = { "input" };
			const char* out_names[] = { "output" };
			OrtStatus* st = api_->Run(session_, nullptr, in_names, &in_val, 1, out_names, 1, &out_val);
			api_->ReleaseValue(in_val);
			if (!check(st, err))
			{
				return false;
			}
			float* data = nullptr;
			if (!check(api_->GetTensorMutableData(out_val, (void**)&data), err))
			{
				api_->ReleaseValue(out_val);
				return false;
			}
			std::memcpy(out, data, count * sizeof(float));
			api_->ReleaseValue(out_val);
			return true;
		}

		void close()
		{
			if (api_)
			{
				if (mem_)
				{
					api_->ReleaseMemoryInfo(mem_);
				}
				if (session_)
				{
					api_->ReleaseSession(session_);
				}
				if (opts_)
				{
					api_->ReleaseSessionOptions(opts_);
				}
				if (env_)
				{
					api_->ReleaseEnv(env_);
				}
			}
			mem_ = nullptr;
			session_ = nullptr;
			opts_ = nullptr;
			env_ = nullptr;
			api_ = nullptr;
			if (lib_)
			{
#ifdef _WIN32
				FreeLibrary((HMODULE)lib_);
#else
				dlclose(lib_);
#endif
				lib_ = nullptr;
			}
		}

	private:
		void ignore(OrtStatus* st)
		{
			if (st)
			{
				api_->ReleaseStatus(st);
			}
		}

		bool check(OrtStatus* st, std::string& err)
		{
			if (!st)
			{
				return true;
			}
			err = api_->GetErrorMessage(st);
			api_->ReleaseStatus(st);
			return false;
		}

		void* lib_ = nullptr;
		const OrtApi* api_ = nullptr;
		OrtEnv* env_ = nullptr;
		OrtSessionOptions* opts_ = nullptr;
		OrtSession* session_ = nullptr;
		OrtMemoryInfo* mem_ = nullptr;
		bool using_gpu_ = false;
	};

	//---------------------------------------------------------------------
	//	MDX-Net による声の抽出 (44.1kHz ステレオ)
	//---------------------------------------------------------------------
	struct Params
	{
		int n_fft = 7680;
		int dim_f = 3072;
		int dim_t = 256;
		int hop = 1024;
		float compensate = 1.021f;
	};

	// progress(完了したチャンク数, 全体のチャンク数) が false を返したら中止します
	inline bool separate(const std::vector<float>& left, const std::vector<float>& right, std::vector<float>& out_left, std::vector<float>& out_right,
		const Params& prm, OrtRunner& runner, const std::function<bool(int, int)>& progress, std::string& err)
	{
		const int n_fft = prm.n_fft;
		const int hop = prm.hop;
		const int dim_f = prm.dim_f;
		const int dim_t = prm.dim_t;
		const int bins = n_fft / 2 + 1;
		const int chunk = hop * (dim_t - 1);
		const int trim = n_fft / 2;
		const int gen = chunk - 2 * trim;
		const int64_t total = (int64_t)left.size();
		if (gen <= 0 || total <= 0)
		{
			err = "音声が空です";
			return false;
		}
		const int64_t n_chunks = (total + gen - 1) / gen;

		const double pi = 3.14159265358979323846;
		std::vector<float> win((size_t)n_fft);
		for (int i = 0; i < n_fft; i++)
		{
			win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2.0 * pi * i / n_fft));	// 周期ハン窓
		}
		// 逆変換の窓補正 (チャンクごとに同じ)
		const int out_len = hop * (dim_t - 1) + n_fft;
		std::vector<float> wsum((size_t)out_len, 0.0f);
		for (int f = 0; f < dim_t; f++)
		{
			for (int i = 0; i < n_fft; i++)
			{
				wsum[(size_t)f * hop + i] += win[(size_t)i] * win[(size_t)i];
			}
		}

		out_left.assign((size_t)total, 0.0f);
		out_right.assign((size_t)total, 0.0f);

		std::vector<float> xp((size_t)chunk + n_fft);
		std::vector<float> frames((size_t)dim_t * n_fft);
		std::vector<std::complex<float>> spec((size_t)dim_t * bins);
		std::vector<float> in((size_t)4 * dim_f * dim_t);
		std::vector<float> outp((size_t)4 * dim_f * dim_t);
		std::vector<float> recon((size_t)dim_t * n_fft);
		std::vector<float> ola((size_t)out_len);

		pocketfft::shape_t shape_in{ (size_t)dim_t, (size_t)n_fft };
		pocketfft::stride_t stride_real{ (ptrdiff_t)(n_fft * sizeof(float)), (ptrdiff_t)sizeof(float) };
		pocketfft::stride_t stride_cplx{ (ptrdiff_t)(bins * sizeof(std::complex<float>)), (ptrdiff_t)sizeof(std::complex<float>) };
		pocketfft::shape_t axes{ 1 };

		const std::vector<float>* src[2] = { &left, &right };
		for (int64_t ci = 0; ci < n_chunks; ci++)
		{
			if (progress && !progress((int)ci, (int)n_chunks))
			{
				err = "中止しました";
				return false;
			}
			// このチャンクの開始位置 (先頭に trim 分の無音を足した座標系)
			int64_t pos = ci * gen - trim;
			for (int ch = 0; ch < 2; ch++)
			{
				const std::vector<float>& s = *src[ch];
				// 中央(center=True)の反射パディングを含めたチャンクを作る
				auto sample = [&](int64_t idx) -> float
					{
						return (idx >= 0 && idx < total) ? s[(size_t)idx] : 0.0f;
					};
				std::vector<float> c((size_t)chunk);
				for (int i = 0; i < chunk; i++)
				{
					c[(size_t)i] = sample(pos + i);
				}
				int pad = n_fft / 2;
				for (int i = 0; i < pad; i++)
				{
					xp[(size_t)(pad - 1 - i)] = c[(size_t)(1 + i)];
					xp[(size_t)(pad + chunk + i)] = c[(size_t)(chunk - 2 - i)];
				}
				for (int i = 0; i < chunk; i++)
				{
					xp[(size_t)(pad + i)] = c[(size_t)i];
				}
				for (int f = 0; f < dim_t; f++)
				{
					const float* x = &xp[(size_t)f * hop];
					float* d = &frames[(size_t)f * n_fft];
					for (int i = 0; i < n_fft; i++)
					{
						d[i] = x[i] * win[(size_t)i];
					}
				}
				pocketfft::r2c<float>(shape_in, stride_real, stride_cplx, axes, pocketfft::FORWARD, frames.data(), spec.data(), 1.0f, 1);
				for (int k = 0; k < dim_f; k++)
				{
					for (int f = 0; f < dim_t; f++)
					{
						std::complex<float> v = spec[(size_t)f * bins + k];
						in[((size_t)(ch * 2 + 0) * dim_f + k) * dim_t + f] = v.real();
						in[((size_t)(ch * 2 + 1) * dim_f + k) * dim_t + f] = v.imag();
					}
				}
			}

			if (!runner.run(in.data(), outp.data(), dim_f, dim_t, err))
			{
				return false;
			}

			for (int ch = 0; ch < 2; ch++)
			{
				std::fill(spec.begin(), spec.end(), std::complex<float>(0.0f, 0.0f));
				for (int k = 0; k < dim_f; k++)
				{
					for (int f = 0; f < dim_t; f++)
					{
						float re = outp[((size_t)(ch * 2 + 0) * dim_f + k) * dim_t + f];
						float im = outp[((size_t)(ch * 2 + 1) * dim_f + k) * dim_t + f];
						spec[(size_t)f * bins + k] = std::complex<float>(re, im);
					}
				}
				pocketfft::c2r<float>(shape_in, stride_cplx, stride_real, axes, pocketfft::BACKWARD, spec.data(), recon.data(), 1.0f / (float)n_fft, 1);
				std::fill(ola.begin(), ola.end(), 0.0f);
				for (int f = 0; f < dim_t; f++)
				{
					const float* r = &recon[(size_t)f * n_fft];
					float* o = &ola[(size_t)f * hop];
					for (int i = 0; i < n_fft; i++)
					{
						o[i] += r[i] * win[(size_t)i];
					}
				}
				// center=True の分(n_fft/2)を除き、さらに両端の trim を除いた中央部分だけを使う
				std::vector<float>& dst = (ch == 0) ? out_left : out_right;
				for (int i = 0; i < gen; i++)
				{
					int64_t gi = ci * gen + i;
					if (gi >= total)
					{
						break;
					}
					int idx = n_fft / 2 + trim + i;
					float w = std::max(wsum[(size_t)idx], 1e-8f);
					dst[(size_t)gi] = ola[(size_t)idx] / w * prm.compensate;
				}
			}
		}
		if (progress)
		{
			progress((int)n_chunks, (int)n_chunks);
		}
		return true;
	}
}