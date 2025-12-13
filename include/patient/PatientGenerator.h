//
// Created by MattFor on 13/12/2025.
//

#ifndef SOR_PATIENT_GENERATOR_H
#define SOR_PATIENT_GENERATOR_H

#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <random>
#include <optional>
#include <functional>

#include "Patient.h"

struct PatientGeneratorConfig
{
	int total = 1;            // total patients to generate
	int children = 0;         // number of children (absolute)
	int vips = 0;             // number of VIPs (absolute)
	unsigned interval_ms = 0; // 0 == immediate (generate all at once)
	bool allow_process_spawn = true;
	std::string exec_path;               // path to _patient executable
	std::vector<std::string> exec_args; // optional extra args
	std::string logger_host = "127.0.0.1";
	unsigned short logger_port = 7878;
	std::optional<unsigned long> rng_seed;
};

class PatientGenerator
{
public:
	using LoggerFn = std::function<void(const std::string&)>;

//	explicit PatientGenerator(PatientGeneratorConfig cfg, LoggerFn logger);
//
//	~PatientGenerator();
//
//	void start(); // start generator thread
//	void stop() noexcept;
//	void join();
//
//	int generated_count() const noexcept;
//	std::vector<Patient> snapshot_patients() const;

private:
	PatientGeneratorConfig cfg_;
	LoggerFn logger_;
	std::vector<Patient> patients_;
	mutable std::mutex patients_mutex_;

	std::thread worker_;
	std::atomic_bool stop_requested_{false};
	std::atomic_int generated_{0};

	std::mt19937 rng_;
	std::vector<std::string> first_names_;
	std::vector<std::string> last_names_;
	std::vector<std::string> symptom_pool_;
//
//	void init_pools();
//	void run_loop();
//	PatientData make_patient_data(int idx, bool make_child, bool make_vip);
//	std::string make_new_patient_json(const PatientData& pd) const;
//	void spawn_patient_process(const PatientData& pd);
};

#endif // SOR_PATIENT_GENERATOR_H
