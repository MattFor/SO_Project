//
// Created by MattFor on 13/12/2025.
//

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "patient/PatientGenerator.h"
//
//#if defined(_WIN32)
//
//#include <windows.h>
//
//#else
//#include <unistd.h>
//#include <sys/types.h>
//#include <sys/wait.h>
//#endif
//
//PatientGenerator::PatientGenerator(PatientGeneratorConfig cfg, LoggerFn logger)
//		: cfg_(std::move(cfg)), logger_(std::move(logger))
//{
//	if (cfg_.rng_seed.has_value())
//	{
//		rng_.seed(*cfg_.rng_seed);
//	}
//	else
//	{
//		rng_.seed(static_cast<unsigned long>(std::random_device{}()));
//	}
//
//	init_pools();
//	if (cfg_.total > 0)
//	{
//		patients_.reserve(static_cast<size_t>(cfg_.total));
//	}
//}
//
//PatientGenerator::~PatientGenerator()
//{
//	stop();
//	join();
//}
//
//void PatientGenerator::init_pools()
//{
//	first_names_ = {"Jan", "Anna", "Piotr", "Katarzyna", "Marek", "Ewa", "Tomasz", "Agnieszka", "Adam", "Magda", "Jakub", "Maria", "Pawel", "Ola"};
//	last_names_ = {"Kowalski", "Nowak", "Wisniewski", "Wojcik", "Kowalczyk", "Kaminski", "Lewandowski", "Zielinski", "Szymanski"};
//	symptom_pool_ = {"mild cough", "high fever", "chest pain", "headache", "nausea", "broken arm", "laceration", "shortness of breath", "dizziness", "abdominal pain",
//	                 "sore throat", "rash"};
//}
//
//void PatientGenerator::start()
//{
//	if (worker_.joinable())
//	{
//		return;
//	}
//	stop_requested_.store(false, std::memory_order_release);
//	worker_ = std::thread([this] { run_loop(); });
//}
//
//void PatientGenerator::stop() noexcept
//{
//	stop_requested_.store(true, std::memory_order_release);
//}
//
//void PatientGenerator::join()
//{
//	if (worker_.joinable())
//	{
//		worker_.join();
//	}
//}
//
//int PatientGenerator::generated_count() const noexcept
//{
//	return generated_.load(std::memory_order_acquire);
//}
//
//std::vector <Patient> PatientGenerator::snapshot_patients() const
//{
//	std::lock_guard lk(patients_mutex_);
//	return patients_;
//}
//
//PatientData PatientGenerator::make_patient_data(int idx, bool make_child, bool make_vip)
//{
//	PatientData pd;
//	std::ostringstream idoss;
//	idoss << "gen_" << std::setw(4) << std::setfill('0') << (idx + 1);
//	pd.id = idoss.str();
//
//	std::uniform_int_distribution <int> fn(0, int(first_names_.size() - 1));
//	std::uniform_int_distribution <int> ln(0, int(last_names_.size() - 1));
//	pd.name = first_names_[fn(rng_)] + " " + last_names_[ln(rng_)];
//
//	pd.age = make_child ? static_cast<unsigned int>(1 + rng_() % 17) : static_cast<unsigned int>(18 + rng_() % 68);
//	pd.vip = make_vip;
//
//	std::uniform_int_distribution <int> sym_count(1, 2);
//	std::ostringstream symoss;
//	for (int i = 0; i < sym_count(rng_); ++i)
//	{
//		if (i)
//		{
//			symoss << ", ";
//		}
//		symoss << symptom_pool_[rng_() % symptom_pool_.size()];
//	}
//
//	pd.symptoms = symoss.str();
//	return pd;
//}
//
//std::string PatientGenerator::make_new_patient_json(const PatientData& pd) const
//{
//	std::ostringstream ss;
//	auto escape = [](const std::string& in)
//	{
//		std::string out;
//		for (char c: in)
//		{
//			if (c == '\\')
//			{
//				out += "\\\\";
//			}
//			else if (c == '"')
//			{
//				out += "\\\"";
//			}
//			else
//			{
//				out += c;
//			}
//		}
//
//		return out;
//	};
//
//	ss << "{\"cmd\":\"NEW_PATIENT\",\"id\":\"" << escape(pd.id) << "\",\"name\":\"" << escape(pd.name) << "\",\"age\":" << pd.age << ",\"vip\":" << (pd.vip ? "true" : "false")
//	   << ",\"symptoms\":\"" << escape(pd.symptoms) << "\"}";
//
//	return ss.str();
//}
//
//void PatientGenerator::spawn_patient_process(const PatientData& pd)
//{
//#if defined(_WIN32)
//	std::ostringstream cmd;
//	cmd << "\"" << cfg_.exec_path << "\" ";
//	cmd << pd.id << " \"" << pd.name << "\" " << pd.age << " " << (pd.vip ? "1" : "0") << " \"" << pd.symptoms << "\" ";
//	cmd << cfg_.logger_host << " " << cfg_.logger_port;
//
//	STARTUPINFOA si{};
//	PROCESS_INFORMATION pi{};
//	si.cb = sizeof(si);
//	if (CreateProcessA(nullptr, const_cast<char*>(cmd.str().c_str()), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
//	{
//		CloseHandle(pi.hProcess);
//		CloseHandle(pi.hThread);
//	}
//	else
//	{
//		if (logger_)
//		{
//			logger_(std::string("{\"cmd\":\"ERROR\",\"msg\":\"Failed to spawn patient ") + pd.id + "\"}");
//		}
//	}
//#else
//	pid_t pid = fork();
//	if(pid==0) {
//		execl(cfg_.exec_path.c_str(), cfg_.exec_path.c_str(), pd.id.c_str(), pd.name.c_str(),
//			  std::to_string(pd.age).c_str(), pd.vip?"1":"0", pd.symptoms.c_str(),
//			  cfg_.logger_host.c_str(), std::to_string(cfg_.logger_port).c_str(), nullptr);
//		_exit(1);
//	}
//	else if(pid<0) {
//		if(logger_) logger_(std::string("{\"cmd\":\"ERROR\",\"msg\":\"Failed to fork patient ") + pd.id + "\"}");
//	}
//#endif
//}
//
//void PatientGenerator::run_loop()
//{
//	int total = std::max(0, cfg_.total);
//	int children_left = std::clamp(cfg_.children, 0, total);
//	int vips_left = std::clamp(cfg_.vips, 0, total);
//
//	std::vector <int> indices(total);
//	for (int i = 0; i < total; ++i)
//	{
//		indices[i] = i;
//	}
//	std::shuffle(indices.begin(), indices.end(), rng_);
//
//	std::vector <char> child_marks(total, 0);
//	std::vector <char> vip_marks(total, 0);
//	for (int i = 0; i < children_left && i < total; ++i) { child_marks[indices[i]] = 1; }
//	for (int i = 0; i < vips_left && i < total; ++i) { vip_marks[indices[(i + children_left) % std::max(1, total)]] = 1; }
//
//	for (int i = 0; i < total && !stop_requested_.load(std::memory_order_acquire); ++i)
//	{
//		PatientData pd = make_patient_data(i, child_marks[i], vip_marks[i]);
//
//		if (logger_)
//		{
//			logger_(make_new_patient_json(pd));
//		}
//
//		if (cfg_.allow_process_spawn && !cfg_.exec_path.empty())
//		{
//			spawn_patient_process(pd);
//		}
//		else
//		{
//			try
//			{
//				Patient p(pd, nullptr, logger_);
//				std::lock_guard lk(patients_mutex_);
//				patients_.push_back(std::move(p));
//			}
//			catch (...)
//			{
//				if (logger_)
//				{
//					logger_(std::string("{\"cmd\":\"ERROR\",\"msg\":\"Failed create patient ") + pd.id + "\"}");
//				}
//			}
//		}
//
//		generated_.fetch_add(1, std::memory_order_release);
//
//		if (cfg_.interval_ms > 0)
//		{
//			unsigned remaining_ms = cfg_.interval_ms;
//			while (remaining_ms > 0 && !stop_requested_.load(std::memory_order_acquire))
//			{
//				const unsigned step = std::min <unsigned>(50, remaining_ms);
//				std::this_thread::sleep_for(std::chrono::milliseconds(step));
//				remaining_ms -= step;
//			}
//		}
//	}
//}
