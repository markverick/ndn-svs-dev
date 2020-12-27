// AUTHOR: Zhaoning Kong
// Email: jonnykong@cs.ucla.edu

#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <ndn-svs/socket.hpp>

class Options {
 public:
  Options() : prefix("/ndn/svs") {}

 public:
  ndn::Name prefix;
  std::string m_id;
};

namespace ndn {
namespace svs {

class Program {
 public:
  explicit Program(const Options &options)
      : m_options(options),
        m_svs(
          ndn::Name("/ndnnew/svs"),
          ndn::Name(m_options.m_id),
          face,
          std::bind(&Program::onMissingData, this, _1)) {
    printf("SVS client %s starts\n", m_options.m_id.c_str());

    // Suppress warning
    Interest::setDefaultCanBePrefix(true);
  }

  ndn::Face face;

  void run() {
    // Create other thread to run
    std::thread thread_svs([this] { face.processEvents(); });

    std::string init_msg = "User " + m_options.m_id + " has joined the groupchat";
    publishMsg(init_msg);

    std::string userInput = "";

    while (true) {
      std::getline(std::cin, userInput);
      publishMsg(userInput);
    }

    thread_svs.join();
  }

 private:
  void onMissingData(const std::vector<ndn::svs::MissingDataInfo>& v) {
    for (size_t i = 0; i < v.size(); i++) {
      for(SeqNo s = v[i].low; s <= v[i].high; ++s) {
        NodeID nid = v[i].nid;
        m_svs.fetchData(nid, s, [nid] (const Data& data) {
            size_t data_size = data.getContent().value_size();
            std::string content_str((char *)data.getContent().value(), data_size);
            content_str = nid + " : " + content_str;
            std::cout << content_str << std::endl;
          });
      }
    }
  }

  void publishMsg(std::string msg) {
    m_svs.publishData(reinterpret_cast<const uint8_t*>(msg.c_str()),
                      msg.size(),
                      time::milliseconds(1000));
  }

  const Options m_options;
  Socket m_svs;
};

}  // namespace svs
}  // namespace ndn

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("Usage: client <prefix>\n");
    exit(1);
  }

  Options opt;
  opt.m_id = argv[1];

  ndn::svs::Program program(opt);
  program.run();
  return 0;
}