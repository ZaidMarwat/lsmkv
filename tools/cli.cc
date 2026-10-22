// Interactive shell for poking at a database.
//   ./build/lsmkv_cli /tmp/mydb
//   > put user:1 zaid
//   > get user:1
//   > scan user: 10

#include <iostream>
#include <sstream>
#include <string>

#include "lsmkv/db.h"

using namespace lsmkv;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <db-dir>\n";
    return 1;
  }
  std::unique_ptr<DB> db;
  Status s = DB::Open(Options(), argv[1], &db);
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }
  std::cout << "lsmkv shell. commands: put k v | get k | del k | scan [prefix] [limit] | "
               "flush | compact | stats | quit\n";
  std::string line;
  while (std::cout << "> " << std::flush, std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd, k, v;
    in >> cmd;
    if (cmd == "put") {
      in >> k;
      std::getline(in >> std::ws, v);
      s = db->Put({}, k, v);
      std::cout << (s.ok() ? "OK" : s.ToString()) << "\n";
    } else if (cmd == "get") {
      in >> k;
      s = db->Get({}, k, &v);
      std::cout << (s.ok() ? v : s.ToString()) << "\n";
    } else if (cmd == "del") {
      in >> k;
      s = db->Delete({}, k);
      std::cout << (s.ok() ? "OK" : s.ToString()) << "\n";
    } else if (cmd == "scan") {
      std::string prefix;
      int limit = 20;
      in >> prefix >> limit;
      auto it = db->NewIterator({});
      int n = 0;
      for (it->Seek(prefix); it->Valid() && n < limit; it->Next(), n++) {
        if (it->key().substr(0, prefix.size()) != prefix) break;
        std::cout << it->key() << " = " << it->value() << "\n";
      }
      std::cout << "(" << n << " rows)\n";
    } else if (cmd == "flush") {
      std::cout << db->Flush().ToString() << "\n";
    } else if (cmd == "compact") {
      std::cout << db->CompactAll().ToString() << "\n";
    } else if (cmd == "stats") {
      std::cout << db->DebugString();
    } else if (cmd == "quit" || cmd == "exit") {
      break;
    } else if (!cmd.empty()) {
      std::cout << "unknown command\n";
    }
  }
  return 0;
}
