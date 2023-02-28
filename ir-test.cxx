#include <string>
#include <regex>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <array>
#include <exception>
#include <cstdlib>

#ifdef _WIN32
# include <windows.h>
# define popen _popen
# define pclose _pclose
# define PATH_SEP "\\"
# define EXE_SUF ".exe"
# define REGEX_FLAGS std::regex::extended
# define DIFF_ARGS "--strip-trailing-cr"
# define IR_ARGS "--no-abort-fault"
#else
# define PATH_SEP "/"
# define EXE_SUF ""
# define REGEX_FLAGS std::regex::ECMAScript | std::regex::multiline
# define DIFF_ARGS ""
# define IR_ARGS ""
#endif

uint32_t skipped = 0;
std::vector<std::string> bad_list;
std::vector<std::tuple<std::string, std::string>> failed;
std::vector<std::tuple<std::string, std::string, std::string>> xfailed_list;
bool show_diff = false, colorize = true;
std::string src_dir, build_dir, test_dir, ir_exe, ir_target;

namespace ir {
	decltype(auto) trim(std::string s) {
		const char* ws = " \t\n\r\f\v";
		s.erase(0, s.find_first_not_of(ws));
		s.erase(s.find_last_not_of(ws) + 1);
		return s;
	}

	decltype(auto) exec(const std::string& cmd) {
		std::array<char, 256> buffer;
		std::string result;
		std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
		if (!pipe) {
			throw std::runtime_error("popen() failed!");
		}
		while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
			result += buffer.data();
		}
		return result;
	}

	decltype(auto) get_dir_from_env(const char* name) {
		const char* _tmp = std::getenv(name);
		std::string ret = _tmp ? _tmp : ".";
		return ret;
	}

	enum color { GREEN, YELLOW, RED };

	decltype(auto) colorize(const std::string& s, enum color c) {
		if (::colorize)
			switch (c) {
				case GREEN:
					return "\x1b[1;32m" + s + "\x1b[0m";
				case YELLOW:
					return "\x1b[1;33m" + s + "\x1b[0m";
				case RED:
					return "\x1b[1;31m" + s + "\x1b[0m";
			}
		return "" + s;
	}

	void init_console() {
#ifdef _WIN32
		if (::colorize) {
			DWORD mode, new_mode = ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
			HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
			GetConsoleMode(h, &mode);
			SetConsoleMode(h, mode | new_mode);
		}
#endif
	}

	class broken_test_exception : public std::exception {} broken_test;

	struct it_sect {
		std::size_t name_pos;
		std::string name;
		std::size_t content_pos;
		std::string content;
	};

	struct test {
		// Section contents
		std::string name, code, expect, target, xfail, args;
		// Test file path and contents and output filenames
		std::string irt_file, irt_file_content, ir_file, out_file, exp_file, diff_file;
		test(const std::string& test_fl) : irt_file{test_fl} {
			std::ifstream f(test_fl);
			if (!f) throw "Couldn't read '" + test_fl + "'";
			std::ostringstream oss;
			oss << f.rdbuf();
			irt_file_content = oss.str();

			auto make_fn = [&test_fl](const std::string& s) {
				return test_fl.substr(0, test_fl.length() - 4) + s;
			};
			ir_file = make_fn(".ir");
			out_file = make_fn(".out");
			exp_file = make_fn(".exp");
			diff_file = make_fn(".diff");

			parse();
		}
		void parse() {
			std::regex sect_reg("^--[A-Z]+--$", REGEX_FLAGS);
			auto sect_begin = std::sregex_iterator(irt_file_content.begin(), irt_file_content.end(), sect_reg);
			auto sect_end = std::sregex_iterator();
			std::vector<it_sect> test_data;

			// PASS 0: Find all the test sections
			auto found_sects = std::distance(sect_begin, sect_end);
			for (auto i = sect_begin; i != sect_end; i++) {
				it_sect iti;
				std::smatch m = *i;
				iti.name_pos = m.position();
				iti.name = m.str();
				test_data.push_back(std::move(iti));
			}
			// PASS 1: Read section contents
			auto it = test_data.begin();
			auto it_next = it;
			it_next++;
			for (; it != test_data.end(); it++) {
				it->content_pos = it->name_pos + it->name.length();

				if (test_data.end() == it_next) {
					it->content = trim(irt_file_content.substr(it->content_pos, irt_file_content.length() - it->content_pos));
				} else {
					it->content = trim(irt_file_content.substr(it->content_pos, it_next->name_pos - it->content_pos));
					it_next++;
				}
			}

			//  PASS 2: Save necessary data into properties
#define NO_SECT test_data.end()
			auto get_sect = [&test_data](std::string s) -> decltype(auto) {
				for (auto test_it = test_data.begin(); test_it != test_data.end(); test_it++)
					if (!test_it->name.compare(s)) return test_it;
				return NO_SECT;
			};
			auto test_sect = get_sect("--TEST--");
			auto code_sect = get_sect("--CODE--");
			auto exp_sect = get_sect("--EXPECT--");
			if (NO_SECT == test_sect || NO_SECT == code_sect || NO_SECT == exp_sect) {
				throw broken_test;
			}
			name = test_sect->content;
			code = code_sect->content;
			expect = exp_sect->content;
			auto args_sect = get_sect("--ARGS--");
			args = (NO_SECT == args_sect) ? "--save": args_sect->content;
			auto tgt_sect = get_sect("--TARGET--");
			if (NO_SECT != tgt_sect) target = tgt_sect->content;
			auto xfail_sect = get_sect("--XFAIL--");
			if (NO_SECT != xfail_sect) xfail = xfail_sect->content;
		}
		bool skip() {
			return target.length() > 0 && target.compare(::ir_target);
		}
		bool run() {
			std::remove(out_file.c_str());
			std::remove(exp_file.c_str());
			std::remove(diff_file.c_str());
			std::remove(ir_file.c_str());

			std::ofstream in_os(ir_file);
			in_os << code;
			in_os.close();

			auto test_cmd = ::ir_exe + " " + ir_file + " " + args + " " + IR_ARGS + " >" + out_file + " 2>&1";
			auto diff_cmd = std::string("diff") + " " + DIFF_ARGS + "-u " + exp_file + " " + out_file + " > " + diff_file + " 2>&1";

			int ret_code = std::system(test_cmd.c_str()) >> 0x8;
			if (ret_code) {
				return false;
			}

			std::stringstream out_buf;
			out_buf << std::ifstream(out_file).rdbuf();
			std::string out = trim(out_buf.str());
			out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());

			if (out.compare(expect)) {
				std::ofstream exp_os(exp_file);
				exp_os << (expect + "\n");
				exp_os.close();

				// XXX move away from diff tool dependency
				ret_code = std::system(diff_cmd.c_str()) >> 0x8;
				if (0 != ret_code && 1 != ret_code) { // 1 stands for "there is a diff"
					std::cout << diff_cmd << std::endl;
					std::cout << ret_code << std::endl;
					throw "Couldn't compare output vs. expected result for '" + irt_file + "'";
				}
				return false;
			}

			return true;
		}
	};

	void find_tests_in_dir(std::string& test_dir, std::vector<std::string>& irt_files) {
		for (const std::filesystem::directory_entry& ent :
		        std::filesystem::recursive_directory_iterator(test_dir)) {
			std::string fl(ent.path().string());
			if (fl.length() < 4 || 0 != fl.compare(fl.length()-4, 4, ".irt")) continue;
			irt_files.push_back(fl);
		}
		std::sort(irt_files.begin(), irt_files.end(), [&](const std::string& a, const std::string& b) {
			return 1 <= b.compare(a);
		});
	}
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		// XXX use some cleaner arg parsing solution
		if (!std::string(argv[i]).compare("--show-diff")) {
			::show_diff = true;
		} else if (!std::string(argv[i]).compare("--no-color")) {
			::colorize = false;
		}
	}
	ir::init_console();

	::build_dir = ir::get_dir_from_env("BUILD_DIR");
	::src_dir = ir::get_dir_from_env("SRC_DIR");
	::test_dir = ::src_dir + PATH_SEP + "tests";
	::ir_exe = ::build_dir + PATH_SEP + "ir" + EXE_SUF;
	::ir_target = ir::trim(ir::exec(::ir_exe + " --target"));
	std::vector<std::string> irt_files;

	ir::find_tests_in_dir(::test_dir, irt_files);
	for (const std::string& test_fl : irt_files) {
		try {
			auto test = ir::test(test_fl);

			size_t idx = &test_fl - &irt_files[0] + 1;
			std::string test_ln_0 = "TEST: " + std::to_string(idx) + PATH_SEP + std::to_string(irt_files.size()) + " " + test.name + "[" + test_fl + "]\r";
			std::cout << test_ln_0 << std::flush;

			if (test.skip()) {
				std::cout << std::string(test_ln_0.length(), ' ');
				std::cout << "\r" << ir::colorize("SKIP", ir::YELLOW) << ": " << test.name << " [" << test_fl << "]\n";
				::skipped++;
				continue;
			}

			auto ret = test.run();
			std::cout << std::string(test_ln_0.length(), ' ');

			if (ret) {
				std::cout << "\r" << ir::colorize("PASS", ir::GREEN) << ": " << test.name << " [" << test_fl << "]\n";
			} else if (test.xfail.length() > 0) {
				std::cout << "\r" << ir::colorize("XFAIL", ir::RED) << ": " << test.name << " [" << test_fl << "]  XFAIL REASON: " << test.xfail << "\n";
				::xfailed_list.push_back({test.name, test.irt_file, test.xfail});
			} else {
				std::cout << "\r" << ir::colorize("FAIL", ir::RED) << ": " << test.name << " [" << test_fl << "]\n";
				::failed.push_back({test.name, test.irt_file});
				if (::show_diff) {
					std::ifstream f(test.diff_file);
					if (!f) {
						throw "Couldn't read '" + test.diff_file + "'";
					}
					std::cout << f.rdbuf() << std::endl;
					f.close();
				}
			}
		} catch (ir::broken_test_exception& e) {
			std::cout << "\r" << ir::colorize("BROK", ir::RED) << ": [" << test_fl << "]\n";
			::bad_list.push_back(test_fl);
		} catch (std::string& s) {
			std::cout << "\r" << ir::colorize("ERROR", ir::RED) << ": " << s << '\n';
		} catch (std::exception& e) {
			std::cout << "\r" << ir::colorize("ERROR", ir::RED) << ": " << e.what() << '\n';
		}

		// XXX parallelize
	}

	// Produce the summary
#define SEPARATOR() std::cout << std::string(32, '-') << std::endl
#define WRAP_OUT(expr) SEPARATOR(); expr; SEPARATOR()
	WRAP_OUT(std::cout << "Test Summary" << std::endl);
	if (::bad_list.size() > 0) {
		std::cout << "Bad tests: " << ::bad_list.size() << std::endl;
		WRAP_OUT(for (const std::string& fname : ::bad_list) std::cout << fname << std::endl);
	}
	std::cout << "Total: " << irt_files.size() << std::endl;
	std::cout << "Passed: " << (irt_files.size() - ::failed.size() - ::skipped) << std::endl;
	std::cout << "Expected fail: " << ::xfailed_list.size() << std::endl;
	std::cout << "Failed: " << ::failed.size() << std::endl;
	std::cout << "Skipped: " << ::skipped << std::endl;
	if (::xfailed_list.size() > 0) {
		WRAP_OUT(std::cout << "EXPECTED FAILED TESTS" << std::endl);
		for (const auto& t : ::xfailed_list) {
			std::cout << std::get<0>(t) << " [" << std::get<1>(t) << "]  XFAIL REASON: " << std::get<2>(t) << std::endl;
		}
	}
	if (::failed.size() > 0) {
		WRAP_OUT(std::cout << "FAILED TESTS" << std::endl);
		for (const auto& t : ::failed) {
			std::cout << std::get<0>(t) << " [" << std::get<1>(t) << "]" << std::endl;
		}
	}
	SEPARATOR();

	return ::failed.size() > 0 ? 1 : 0;
}