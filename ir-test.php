<?php
/*
 * IR - Lightweight JIT Compilation Framework
 * (Test driver)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

function parse_test($test, &$name, &$code, &$expect, &$args, &$target) {
	$text = @file_get_contents($test);
	if (!$text) {
		return false;
	}
	$p1 = strpos($text, '--TEST--');
	$p_args = strpos($text, '--ARGS--');
	$p_target = strpos($text, '--TARGET--');
	$p3 = strpos($text, '--CODE--');
	$p4 = strpos($text, '--EXPECT--');
	if ($p1 === false || $p3 === false || $p4 === false || $p1 > $p3 || $p3 > $p4) {
		return false;
	}
	$code = trim(substr($text, $p3 + strlen('--CODE--'), $p4 - $p3 - strlen('--CODE--')));
	$expect = trim(substr($text, $p4 + strlen('--EXPECT--')));
	$expect = str_replace("\r", "", $expect);

	$end_name = $p3;
	$args = "--save";
	$target = null;

	if ($p_args !== false ) {
		$end = ($p_target !== false && $p_target > $p_args) ? $p_target : $p3;
		if ($p_args < $p1 || $p_args > $end) {
			return false;
		}
		if ($p_args < $end_name) {
			$end_name = $p_args;
		}
		$args = trim(substr($text, $p_args + strlen('--ARGS--'), $end - $p_args - strlen('--ARGS--')));
	}

	if ($p_target !== false ) {
		$end = ($p_args !== false && $p_args > $p_target) ? $p_args : $p3;
		if ($p_target < $p1 || $p_target > $end) {
			return false;
		}
		if ($p_target < $end_name) {
			$end_name = $p_target;
		}
		$target = trim(substr($text, $p_target + strlen('--TARGET--'), $end - $p_target - strlen('--TARGET--')));
	}

	$name = trim(substr($text, $p1 + strlen('--TEST--'), $end_name - $p1 - strlen('--TEST--')));

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
	@system("$build_dir/ir $input $args >$output 2>&1");
//	if (@system("$build_dir/ir $input $args 2>&1 >$output") != 0) {
//		return false;
//	}
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
		if (@system("diff -u $base.exp $output > $base.diff") != 0) {
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
	$build_dir = getenv("BUILD_DIR") ?? ".";
	$src_dir = getenv("SRC_DIR") ?? ".";
	$skiped = 0;
    $target = @system("$build_dir/ir --target");
	$tests = find_tests("$src_dir/tests");
	$bad = array();
	$failed = array();
	$total = count($tests);
	$count = 0;
	foreach($tests as $test) {
		$count++;
		if (parse_test($test, $name, $code, $expect, $opt, $test_target)) {
			if ($test_target !== null && $target != $test_target) {
				echo "\r\e[1;33mSKIP\e[0m: $name [$test]\n";
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
				echo "\r\e[1;32mPASS\e[0m: $name [$test]\n";
			} else {
				echo "\r\e[1;31mFAIL\e[0m: $name [$test]\n";
				$failed[$test] = $name;
				global $show_diff;
				if ($show_diff) {
					$base   = substr($test, 0, -4);
					echo file_get_contents("$base.diff");
				}
			}
		} else {
			echo "\r\e[1;31mBROK\e[0m: $name [$test]\n";
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
	echo "Failed: " . count($failed) . "\n";
	echo "Skiped: " . $skiped . "\n";
	if (count($failed) > 0) {
		echo "-------------------------------\n";
		foreach ($failed as $test => $name) {
			echo "$name [$test]\n";
		}
	}
	echo "-------------------------------\n";

	return count($failed) > 0 ? 1 : 0;
}

$show_diff = in_array('--show-diff', $argv, true);
exit(run_tests());
