#pragma once
#include "json.hpp"
#include <memory>
#include <mutex>
#include <string>

using namespace std;

class ConfigError : public std::exception {
  string err;

public:
  ConfigError(const string &error) : err(error) {}
  const char *what() const noexcept override { return err.c_str(); }
};

// class ConfigError:public std::exception{
// 	string err;
// public:
// 	ConfigError(const string& error):err(error){}
// 	const char * what() const noexcept override{
// 		return err.c_str();
// 	}
// };
//
// class Config
// {
// public:
// 	nlohmann::json config;
//
// 	void loadcfg(const char* fn, nlohmann::json& config)
// 	{
// 		stringstream cfg(fn);
// 		if(!cfg.good())
// 			throw ConfigError("Config leer");
// 		cfg>>config;
// 	}
// };
