<?php
/*
 * IR - Lightweight JIT Compilation Framework
 * (Test driver)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

function parse_test($test, &$name, &$code, &$expect, &$args, &$target, &$xfail) {
	$sections = array();
	$text = @file_get_contents($test);
	if (!$text || !preg_match_all("/^--[A-Z]+--$/m", $text, $r, PREG_SET_ORDER | PREG_OFFSET_CAPTURE)) {
		return false;
	}
	foreach($r as $sect) {
		if (isset($sect_name)) {
			$sections[$sect_name] = trim(substr($text, $sect_offset, $sect[0][1] - $sect_offset));
		}
		$sect_name = $sect[0][0];
		$sect_offset = $sect[0][1] + strlen($sect_name);
	}
	if (isset($sect_name)) {
		$sections[$sect_name] = trim(substr($text, $sect_offset));
	}
	if (!isset($sections['--TEST--']) || !isset($sections['--CODE--']) || !isset($sections['--EXPECT--'])) {
		return false;
	}
	$name = $sections['--TEST--'];
	$code = $sections['--CODE--'];
	$expect = $sections['--EXPECT--'];
	$args = isset($sections['--ARGS--']) ? $sections['--ARGS--'] : "--save";
	$target = isset($sections['--TARGET--']) ? $sections['--TARGET--'] : null;
	$xfail = isset($sections['--XFAIL--']) ? $sections['--XFAIL--'] : null;
	return true;
}

function run_test($build_dir, $test, $name, $code, $expect, $args) {
	$base   = substr($test, 0, -4);
	$input = $base . ".ir";
	$output = $base . ".out";
	@unlink($input);
	@unlink($output);
	@unlink("$base.exp");
	@unlink("$base.diff");
	if (!@file_put_contents($input, $code)) {
		return false;
	}
	if (PHP_OS_FAMILY != "Windows") {
		$cmd = "$build_dir/ir $input $args >$output 2>&1";
	} else {
		$cmd = "$build_dir\\ir $input $args --no-abort-fault >$output 2>&1";
	}
	$ret = @system($cmd);
	if ($ret === false) {
		return false;
	}
	$out = @file_get_contents($output);
	if ($out === false) {
		return false;
	}
	$out = trim($out);
	$out = str_replace("\r", "", $out);
	if ($out !== $expect) {
		if (!@file_put_contents("$base.exp", "$expect\n")) {
			return false;
		}
		if (PHP_OS_FAMILY != "Windows") {
			$cmd = "diff -u $base.exp $output > $base.diff";
		} else {
			/* Diff somehow resets terminal and breaks "cooring" */
			$cmd = "diff --strip-trailing-cr -u $base.exp $output > $base.diff 2>&1";
		}
		if (@system($cmd) != 0) {
			return false;
		}
		return false;
	}
	@unlink($input);
	@unlink($output);
	return true;
}

function find_tests_in_dir($dir, &$tests) {
	$d = opendir($dir);
	if ($d !== false) {
		while (($name = readdir($d)) !== false) {
			if ($name  === '.' || $name === '..') continue;
			$fn = "$dir/$name";
			if (is_dir($fn)) {
				find_tests_in_dir($fn, $tests);
			} else if (substr($name, -4) === '.irt') {
				$tests[] = $fn;
			}
		}
		closedir($d);
	}
}

function find_tests($dir) {
    $tests = [];
	find_tests_in_dir($dir, $tests);
	sort($tests);
	return $tests;
}

function run_tests() {
	global $show_diff, $colorize;

	$build_dir = getenv("BUILD_DIR") ?? ".";
	$src_dir = getenv("SRC_DIR") ?? ".";
	$skiped = 0;
	if (PHP_OS_FAMILY != "Windows") {
		$cmd = "$build_dir/ir --target";
	} else {
		$cmd = "$build_dir\\ir --target";
	}
	$target = @system($cmd);
	if ($target === false) {
		echo "Cannot run '$cmd'\n";
		return 1;
	}
	$tests = find_tests("$src_dir/tests");
	$bad = array();
	$failed = array();
	$xfailed = array();
	$total = count($tests);
	$count = 0;
	foreach($tests as $test) {
		$count++;
		if (parse_test($test, $name, $code, $expect, $opt, $test_target, $xfail)) {
			if ($test_target !== null && $target != $test_target) {
				if ($colorize) {
					echo "\r\e[1;33mSKIP\e[0m: $name [$test]\n";
				} else {
					echo "\rSKIP: $name [$test]\n";
				}
				$skiped++;
				continue;
			}
			$str = "TEST: $count/$total $name [$test]\r";
			$len = strlen($str);
			echo $str;
			flush();
			$ret = run_test($build_dir, $test, $name, $code, $expect, $opt);
			echo str_repeat(" ", $len);
			if ($ret) {
				if ($colorize) {
					echo "\r\e[1;32mPASS\e[0m: $name [$test]\n";
				} else {
					echo "\rPASS: $name [$test]\n";
				}
			} else if (isset($xfail)) {
				if ($colorize) {
					echo "\r\e[1;31mXFAIL\e[0m: $name [$test]  XFAIL REASON: $xfail\n";
				} else {
					echo "\rXFAIL: $name [$test]  XFAIL REASON: $xfail\n";
				}
				$xfailed[$test] = array($name, $xfail);
			} else {
				if ($colorize) {
					echo "\r\e[1;31mFAIL\e[0m: $name [$test]\n";
				} else {
					echo "\rFAIL: $name [$test]\n";
				}
				$failed[$test] = $name;
				if ($show_diff) {
					$base   = substr($test, 0, -4);
					echo file_get_contents("$base.diff");
				}
			}
		} else {
			if ($colorize) {
				echo "\r\e[1;31mBROK\e[0m: $name [$test]\n";
			} else {
				echo "\rBROK: $name [$test]\n";
			}
			$bad[] = $test;
		}
	}
	echo "-------------------------------\n";
	echo "Test Summary\n";
	echo "-------------------------------\n";
	if (count($bad) > 0) {
		echo "Bad tests:  " . count($bad) . "\n";
		echo "-------------------------------\n";
		foreach ($bad as $test) {
			echo "$test\n";
		}
		echo "-------------------------------\n";
	}
	echo "Total: " . count($tests) . "\n";
	echo "Passed: " . (count($tests) - count($failed) - $skiped) . "\n";
	echo "Expected fail: " . count($xfailed) . "\n";
	echo "Failed: " . count($failed) . "\n";
	echo "Skiped: " . $skiped . "\n";
	if (count($xfailed) > 0) {
		echo "-------------------------------\n";
		echo "EXPECTED FAILED TESTS\n";
		echo "-------------------------------\n";
		foreach ($xfailed as $test => list($name, $xfail)) {
			echo "$name [$test]  XFAIL REASON: $xfail\n";
		}
	}
	if (count($failed) > 0) {
		echo "-------------------------------\n";
		echo "FAILED TESTS\n";
		echo "-------------------------------\n";
		foreach ($failed as $test => $name) {
			echo "$name [$test]\n";
		}
	}
	echo "-------------------------------\n";

	return count($failed) > 0 ? 1 : 0;
}

$colorize = true;
if (function_exists('sapi_windows_vt100_support') && !sapi_windows_vt100_support(STDOUT, true)) {
    $colorize = false;
}
if (in_array('--no-color', $argv, true)) {
    $colorize = false;
}
$show_diff = in_array('--show-diff', $argv, true);
exit(run_tests());
