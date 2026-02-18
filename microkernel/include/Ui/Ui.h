#pragma once
#include "json.hpp"
#include <string>
#include <vector>

struct Call;
struct Ticket;

using namespace std;

class Ui {
public:
  Ui() = default;
  Ui(nlohmann::json &config);
  virtual ~Ui();

  string url;
  virtual string apiToUi(istream &response) = 0;
  virtual string uiToApi(istream &request) = 0;
  virtual string combineCallInfoAndTicketsForDashboard(string &call,
                                                       string &tickets) = 0;
  virtual void sendActionResult(bool success, const string &operation,
                                const string &message,
                                const string &ticketId = "") = 0;

protected:
  template <typename T>
  T getConfigValue(nlohmann::json &config, const char *param,
                   const T &defaultVal, bool &hasError);
};

typedef Ui *(UiSysCreator)(nlohmann::json &config);
