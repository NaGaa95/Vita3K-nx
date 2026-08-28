#pragma once
#include <string>

extern std::string g_forwarderSelfPath;

bool forwarder_create(const std::string &gameKey, const std::string &name,
                      const std::string &iconImgPath, char *err, std::size_t errSize);
bool forwarder_create_launcher(const std::string &iconImgPath,char *err,std::size_t errSize);
