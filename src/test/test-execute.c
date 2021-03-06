/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2014 Ronny Chevalier

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/types.h>

#include "errno-list.h"
#include "fileio.h"
#include "fs-util.h"
#include "macro.h"
#include "manager.h"
#include "mkdir.h"
#include "path-util.h"
#include "rm-rf.h"
#if HAVE_SECCOMP
#include "seccomp-util.h"
#endif
#include "stat-util.h"
#include "test-helper.h"
#include "tests.h"
#include "unit.h"
#include "util.h"
#include "virt.h"

typedef void (*test_function_t)(Manager *m);

static void check(Manager *m, Unit *unit, int status_expected, int code_expected) {
        Service *service = NULL;
        usec_t ts;
        usec_t timeout = 2 * USEC_PER_MINUTE;

        assert_se(m);
        assert_se(unit);

        service = SERVICE(unit);
        printf("%s\n", unit->id);
        exec_context_dump(&service->exec_context, stdout, "\t");
        ts = now(CLOCK_MONOTONIC);
        while (!IN_SET(service->state, SERVICE_DEAD, SERVICE_FAILED)) {
                int r;
                usec_t n;

                r = sd_event_run(m->event, 100 * USEC_PER_MSEC);
                assert_se(r >= 0);

                n = now(CLOCK_MONOTONIC);
                if (ts + timeout < n) {
                        log_error("Test timeout when testing %s", unit->id);
                        exit(EXIT_FAILURE);
                }
        }
        exec_status_dump(&service->main_exec_status, stdout, "\t");
        assert_se(service->main_exec_status.status == status_expected);
        assert_se(service->main_exec_status.code == code_expected);
}

static bool is_inaccessible_available(void) {
        char *p;

        FOREACH_STRING(p,
                "/run/systemd/inaccessible/reg",
                "/run/systemd/inaccessible/dir",
                "/run/systemd/inaccessible/chr",
                "/run/systemd/inaccessible/blk",
                "/run/systemd/inaccessible/fifo",
                "/run/systemd/inaccessible/sock"
        ) {
                if (access(p, F_OK) < 0)
                        return false;
        }

        return true;
}

static void test(Manager *m, const char *unit_name, int status_expected, int code_expected) {
        Unit *unit;

        assert_se(unit_name);

        assert_se(manager_load_unit(m, unit_name, NULL, NULL, &unit) >= 0);
        assert_se(UNIT_VTABLE(unit)->start(unit) >= 0);
        check(m, unit, status_expected, code_expected);
}

static void test_exec_bindpaths(Manager *m) {
        assert_se(mkdir_p("/tmp/test-exec-bindpaths", 0755) >= 0);
        assert_se(mkdir_p("/tmp/test-exec-bindreadonlypaths", 0755) >= 0);

        test(m, "exec-bindpaths.service", 0, CLD_EXITED);

        (void) rm_rf("/tmp/test-exec-bindpaths", REMOVE_ROOT|REMOVE_PHYSICAL);
        (void) rm_rf("/tmp/test-exec-bindreadonlypaths", REMOVE_ROOT|REMOVE_PHYSICAL);
}

static void test_exec_workingdirectory(Manager *m) {
        assert_se(mkdir_p("/tmp/test-exec_workingdirectory", 0755) >= 0);

        test(m, "exec-workingdirectory.service", 0, CLD_EXITED);

        (void) rm_rf("/tmp/test-exec_workingdirectory", REMOVE_ROOT|REMOVE_PHYSICAL);
}

static void test_exec_personality(Manager *m) {
#if defined(__x86_64__)
        test(m, "exec-personality-x86-64.service", 0, CLD_EXITED);

#elif defined(__s390__)
        test(m, "exec-personality-s390.service", 0, CLD_EXITED);

#elif defined(__powerpc64__)
#  if __BYTE_ORDER == __BIG_ENDIAN
        test(m, "exec-personality-ppc64.service", 0, CLD_EXITED);
#  else
        test(m, "exec-personality-ppc64le.service", 0, CLD_EXITED);
#  endif

#elif defined(__aarch64__)
        test(m, "exec-personality-aarch64.service", 0, CLD_EXITED);

#elif defined(__i386__)
        test(m, "exec-personality-x86.service", 0, CLD_EXITED);
#else
        log_notice("Unknown personality, skipping %s", __func__);
#endif
}

static void test_exec_ignoresigpipe(Manager *m) {
        test(m, "exec-ignoresigpipe-yes.service", 0, CLD_EXITED);
        test(m, "exec-ignoresigpipe-no.service", SIGPIPE, CLD_KILLED);
}

static void test_exec_privatetmp(Manager *m) {
        assert_se(touch("/tmp/test-exec_privatetmp") >= 0);

        test(m, "exec-privatetmp-yes.service", 0, CLD_EXITED);
        test(m, "exec-privatetmp-no.service", 0, CLD_EXITED);

        unlink("/tmp/test-exec_privatetmp");
}

static void test_exec_privatedevices(Manager *m) {
        int r;

        if (detect_container() > 0) {
                log_notice("Testing in container, skipping %s", __func__);
                return;
        }
        if (!is_inaccessible_available()) {
                log_notice("Testing without inaccessible, skipping %s", __func__);
                return;
        }

        test(m, "exec-privatedevices-yes.service", 0, CLD_EXITED);
        test(m, "exec-privatedevices-no.service", 0, CLD_EXITED);

        /* We use capsh to test if the capabilities are
         * properly set, so be sure that it exists */
        r = find_binary("capsh", NULL);
        if (r < 0) {
                log_error_errno(r, "Could not find capsh binary, skipping remaining tests in %s: %m", __func__);
                return;
        }

        test(m, "exec-privatedevices-yes-capability-mknod.service", 0, CLD_EXITED);
        test(m, "exec-privatedevices-no-capability-mknod.service", 0, CLD_EXITED);
        test(m, "exec-privatedevices-yes-capability-sys-rawio.service", 0, CLD_EXITED);
        test(m, "exec-privatedevices-no-capability-sys-rawio.service", 0, CLD_EXITED);
}

static void test_exec_protectkernelmodules(Manager *m) {
        int r;

        if (detect_container() > 0) {
                log_notice("Testing in container, skipping %s", __func__);
                return;
        }
        if (!is_inaccessible_available()) {
                log_notice("Testing without inaccessible, skipping %s", __func__);
                return;
        }

        r = find_binary("capsh", NULL);
        if (r < 0) {
                log_error_errno(r, "Skipping %s, could not find capsh binary: %m", __func__);
                return;
        }

        test(m, "exec-protectkernelmodules-no-capabilities.service", 0, CLD_EXITED);
        test(m, "exec-protectkernelmodules-yes-capabilities.service", 0, CLD_EXITED);
        test(m, "exec-protectkernelmodules-yes-mount-propagation.service", 0, CLD_EXITED);
}

static void test_exec_readonlypaths(Manager *m) {

        test(m, "exec-readonlypaths-simple.service", 0, CLD_EXITED);

        if (path_is_read_only_fs("/var") > 0) {
                log_notice("Directory /var is readonly, skipping remaining tests in %s", __func__);
                return;
        }

        test(m, "exec-readonlypaths.service", 0, CLD_EXITED);
        test(m, "exec-readonlypaths-mount-propagation.service", 0, CLD_EXITED);
        test(m, "exec-readonlypaths-with-bindpaths.service", 0, CLD_EXITED);
}

static void test_exec_readwritepaths(Manager *m) {

        if (path_is_read_only_fs("/") > 0) {
                log_notice("Root directory is readonly, skipping %s", __func__);
                return;
        }

        test(m, "exec-readwritepaths-mount-propagation.service", 0, CLD_EXITED);
}

static void test_exec_inaccessiblepaths(Manager *m) {

        if (!is_inaccessible_available()) {
                log_notice("Testing without inaccessible, skipping %s", __func__);
                return;
        }

        test(m, "exec-inaccessiblepaths-proc.service", 0, CLD_EXITED);

        if (path_is_read_only_fs("/") > 0) {
                log_notice("Root directory is readonly, skipping remaining tests in %s", __func__);
                return;
        }

        test(m, "exec-inaccessiblepaths-mount-propagation.service", 0, CLD_EXITED);
}

static void test_exec_systemcallfilter(Manager *m) {
#if HAVE_SECCOMP
        if (!is_seccomp_available()) {
                log_notice("Seccomp not available, skipping %s", __func__);
                return;
        }

        test(m, "exec-systemcallfilter-not-failing.service", 0, CLD_EXITED);
        test(m, "exec-systemcallfilter-not-failing2.service", 0, CLD_EXITED);
        test(m, "exec-systemcallfilter-failing.service", SIGSYS, CLD_KILLED);
        test(m, "exec-systemcallfilter-failing2.service", SIGSYS, CLD_KILLED);
        test(m, "exec-systemcallfilter-with-errno-name.service", errno_from_name("EILSEQ"), CLD_EXITED);
        test(m, "exec-systemcallfilter-with-errno-number.service", 255, CLD_EXITED);
#endif
}

static void test_exec_systemcallerrornumber(Manager *m) {
#if HAVE_SECCOMP
        if (!is_seccomp_available()) {
                log_notice("Seccomp not available, skipping %s", __func__);
                return;
        }

        test(m, "exec-systemcallerrornumber-name.service", errno_from_name("EACCES"), CLD_EXITED);
        test(m, "exec-systemcallerrornumber-number.service", 255, CLD_EXITED);
#endif
}

static void test_exec_restrictnamespaces(Manager *m) {
#if HAVE_SECCOMP
        if (!is_seccomp_available()) {
                log_notice("Seccomp not available, skipping %s", __func__);
                return;
        }

        test(m, "exec-restrictnamespaces-no.service", 0, CLD_EXITED);
        test(m, "exec-restrictnamespaces-yes.service", 1, CLD_EXITED);
        test(m, "exec-restrictnamespaces-mnt.service", 0, CLD_EXITED);
        test(m, "exec-restrictnamespaces-mnt-blacklist.service", 1, CLD_EXITED);
#endif
}

static void test_exec_systemcallfilter_system(Manager *m) {
#if HAVE_SECCOMP
        if (!is_seccomp_available()) {
                log_notice("Seccomp not available, skipping %s", __func__);
                return;
        }

        if (getpwnam("nobody"))
                test(m, "exec-systemcallfilter-system-user.service", 0, CLD_EXITED);
        else if (getpwnam("nfsnobody"))
                test(m, "exec-systemcallfilter-system-user-nfsnobody.service", 0, CLD_EXITED);
        else
                log_error_errno(errno, "Skipping %s, could not find nobody/nfsnobody user: %m", __func__);
#endif
}

static void test_exec_user(Manager *m) {
        if (getpwnam("nobody"))
                test(m, "exec-user.service", 0, CLD_EXITED);
        else if (getpwnam("nfsnobody"))
                test(m, "exec-user-nfsnobody.service", 0, CLD_EXITED);
        else
                log_error_errno(errno, "Skipping %s, could not find nobody/nfsnobody user: %m", __func__);
}

static void test_exec_group(Manager *m) {
        if (getgrnam("nobody"))
                test(m, "exec-group.service", 0, CLD_EXITED);
        else if (getgrnam("nfsnobody"))
                test(m, "exec-group-nfsnobody.service", 0, CLD_EXITED);
        else
                log_error_errno(errno, "Skipping %s, could not find nobody/nfsnobody group: %m", __func__);
}

static void test_exec_supplementarygroups(Manager *m) {
        test(m, "exec-supplementarygroups.service", 0, CLD_EXITED);
        test(m, "exec-supplementarygroups-single-group.service", 0, CLD_EXITED);
        test(m, "exec-supplementarygroups-single-group-user.service", 0, CLD_EXITED);
        test(m, "exec-supplementarygroups-multiple-groups-default-group-user.service", 0, CLD_EXITED);
        test(m, "exec-supplementarygroups-multiple-groups-withgid.service", 0, CLD_EXITED);
        test(m, "exec-supplementarygroups-multiple-groups-withuid.service", 0, CLD_EXITED);
}

static void test_exec_dynamicuser(Manager *m) {
        test(m, "exec-dynamicuser-fixeduser.service", 0, CLD_EXITED);
        test(m, "exec-dynamicuser-fixeduser-one-supplementarygroup.service", 0, CLD_EXITED);
        test(m, "exec-dynamicuser-supplementarygroups.service", 0, CLD_EXITED);
        test(m, "exec-dynamicuser-statedir.service", 0, CLD_EXITED);

        test(m, "exec-dynamicuser-statedir-migrate-step1.service", 0, CLD_EXITED);
        test(m, "exec-dynamicuser-statedir-migrate-step2.service", 0, CLD_EXITED);
        (void) rm_rf("/var/lib/test-dynamicuser-migrate", REMOVE_ROOT|REMOVE_PHYSICAL);
        (void) rm_rf("/var/lib/test-dynamicuser-migrate2", REMOVE_ROOT|REMOVE_PHYSICAL);
        (void) rm_rf("/var/lib/private/test-dynamicuser-migrate", REMOVE_ROOT|REMOVE_PHYSICAL);
        (void) rm_rf("/var/lib/private/test-dynamicuser-migrate2", REMOVE_ROOT|REMOVE_PHYSICAL);
}

static void test_exec_environment(Manager *m) {
        test(m, "exec-environment.service", 0, CLD_EXITED);
        test(m, "exec-environment-multiple.service", 0, CLD_EXITED);
        test(m, "exec-environment-empty.service", 0, CLD_EXITED);
}

static void test_exec_environmentfile(Manager *m) {
        static const char e[] =
                "VAR1='word1 word2'\n"
                "VAR2=word3 \n"
                "# comment1\n"
                "\n"
                "; comment2\n"
                " ; # comment3\n"
                "line without an equal\n"
                "VAR3='$word 5 6'\n";
        int r;

        r = write_string_file("/tmp/test-exec_environmentfile.conf", e, WRITE_STRING_FILE_CREATE);
        assert_se(r == 0);

        test(m, "exec-environmentfile.service", 0, CLD_EXITED);

        (void) unlink("/tmp/test-exec_environmentfile.conf");
}

static void test_exec_passenvironment(Manager *m) {
        /* test-execute runs under MANAGER_USER which, by default, forwards all
         * variables present in the environment, but only those that are
         * present _at the time it is created_!
         *
         * So these PassEnvironment checks are still expected to work, since we
         * are ensuring the variables are not present at manager creation (they
         * are unset explicitly in main) and are only set here.
         *
         * This is still a good approximation of how a test for MANAGER_SYSTEM
         * would work.
         */
        assert_se(setenv("VAR1", "word1 word2", 1) == 0);
        assert_se(setenv("VAR2", "word3", 1) == 0);
        assert_se(setenv("VAR3", "$word 5 6", 1) == 0);
        test(m, "exec-passenvironment.service", 0, CLD_EXITED);
        test(m, "exec-passenvironment-repeated.service", 0, CLD_EXITED);
        test(m, "exec-passenvironment-empty.service", 0, CLD_EXITED);
        assert_se(unsetenv("VAR1") == 0);
        assert_se(unsetenv("VAR2") == 0);
        assert_se(unsetenv("VAR3") == 0);
        test(m, "exec-passenvironment-absent.service", 0, CLD_EXITED);
}

static void test_exec_umask(Manager *m) {
        test(m, "exec-umask-default.service", 0, CLD_EXITED);
        test(m, "exec-umask-0177.service", 0, CLD_EXITED);
}

static void test_exec_runtimedirectory(Manager *m) {
        test(m, "exec-runtimedirectory.service", 0, CLD_EXITED);
        test(m, "exec-runtimedirectory-mode.service", 0, CLD_EXITED);
        if (getgrnam("nobody"))
                test(m, "exec-runtimedirectory-owner.service", 0, CLD_EXITED);
        else if (getgrnam("nfsnobody"))
                test(m, "exec-runtimedirectory-owner-nfsnobody.service", 0, CLD_EXITED);
        else
                log_error_errno(errno, "Skipping %s, could not find nobody/nfsnobody group: %m", __func__);
}

static void test_exec_capabilityboundingset(Manager *m) {
        int r;

        r = find_binary("capsh", NULL);
        if (r < 0) {
                log_error_errno(r, "Skipping %s, could not find capsh binary: %m", __func__);
                return;
        }

        test(m, "exec-capabilityboundingset-simple.service", 0, CLD_EXITED);
        test(m, "exec-capabilityboundingset-reset.service", 0, CLD_EXITED);
        test(m, "exec-capabilityboundingset-merge.service", 0, CLD_EXITED);
        test(m, "exec-capabilityboundingset-invert.service", 0, CLD_EXITED);
}

static void test_exec_capabilityambientset(Manager *m) {
        int r;

        /* Check if the kernel has support for ambient capabilities. Run
         * the tests only if that's the case. Clearing all ambient
         * capabilities is fine, since we are expecting them to be unset
         * in the first place for the tests. */
        r = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
        if (r < 0 && IN_SET(errno, EINVAL, EOPNOTSUPP, ENOSYS)) {
                log_error("Skipping %s, the kernel does not support ambient capabilities", __func__);
                return;
        }

        if (getpwnam("nobody")) {
                test(m, "exec-capabilityambientset.service", 0, CLD_EXITED);
                test(m, "exec-capabilityambientset-merge.service", 0, CLD_EXITED);
        } else if (getpwnam("nfsnobody")) {
                test(m, "exec-capabilityambientset-nfsnobody.service", 0, CLD_EXITED);
                test(m, "exec-capabilityambientset-merge-nfsnobody.service", 0, CLD_EXITED);
        } else
                log_error_errno(errno, "Skipping %s, could not find nobody/nfsnobody user: %m", __func__);
}

static void test_exec_privatenetwork(Manager *m) {
        int r;

        r = find_binary("ip", NULL);
        if (r < 0) {
                log_error_errno(r, "Skipping %s, could not find ip binary: %m", __func__);
                return;
        }

        test(m, "exec-privatenetwork-yes.service", 0, CLD_EXITED);
}

static void test_exec_oomscoreadjust(Manager *m) {
        test(m, "exec-oomscoreadjust-positive.service", 0, CLD_EXITED);
        test(m, "exec-oomscoreadjust-negative.service", 0, CLD_EXITED);
}

static void test_exec_ioschedulingclass(Manager *m) {
        test(m, "exec-ioschedulingclass-none.service", 0, CLD_EXITED);
        test(m, "exec-ioschedulingclass-idle.service", 0, CLD_EXITED);
        test(m, "exec-ioschedulingclass-realtime.service", 0, CLD_EXITED);
        test(m, "exec-ioschedulingclass-best-effort.service", 0, CLD_EXITED);
}

static void test_exec_unsetenvironment(Manager *m) {
        test(m, "exec-unsetenvironment.service", 0, CLD_EXITED);
}

static void test_exec_specifier(Manager *m) {
        test(m, "exec-specifier.service", 0, CLD_EXITED);
        test(m, "exec-specifier@foo-bar.service", 0, CLD_EXITED);
        test(m, "exec-specifier-interpolation.service", 0, CLD_EXITED);
}

static void test_exec_standardinput(Manager *m) {
        test(m, "exec-standardinput-data.service", 0, CLD_EXITED);
        test(m, "exec-standardinput-file.service", 0, CLD_EXITED);
}

static int run_tests(UnitFileScope scope, const test_function_t *tests) {
        const test_function_t *test = NULL;
        Manager *m = NULL;
        int r;

        assert_se(tests);

        r = manager_new(scope, MANAGER_TEST_RUN_MINIMAL, &m);
        if (MANAGER_SKIP_TEST(r)) {
                log_notice_errno(r, "Skipping test: manager_new: %m");
                return EXIT_TEST_SKIP;
        }
        assert_se(r >= 0);
        assert_se(manager_startup(m, NULL, NULL) >= 0);

        for (test = tests; test && *test; test++)
                (*test)(m);

        manager_free(m);

        return 0;
}

int main(int argc, char *argv[]) {
        static const test_function_t user_tests[] = {
                test_exec_bindpaths,
                test_exec_capabilityambientset,
                test_exec_capabilityboundingset,
                test_exec_environment,
                test_exec_environmentfile,
                test_exec_group,
                test_exec_ignoresigpipe,
                test_exec_inaccessiblepaths,
                test_exec_ioschedulingclass,
                test_exec_oomscoreadjust,
                test_exec_passenvironment,
                test_exec_personality,
                test_exec_privatedevices,
                test_exec_privatenetwork,
                test_exec_privatetmp,
                test_exec_protectkernelmodules,
                test_exec_readonlypaths,
                test_exec_readwritepaths,
                test_exec_restrictnamespaces,
                test_exec_runtimedirectory,
                test_exec_standardinput,
                test_exec_supplementarygroups,
                test_exec_systemcallerrornumber,
                test_exec_systemcallfilter,
                test_exec_umask,
                test_exec_unsetenvironment,
                test_exec_user,
                test_exec_workingdirectory,
                NULL,
        };
        static const test_function_t system_tests[] = {
                test_exec_dynamicuser,
                test_exec_specifier,
                test_exec_systemcallfilter_system,
                NULL,
        };
        int r;

        log_set_max_level(LOG_DEBUG);
        log_parse_environment();
        log_open();

        (void) unsetenv("USER");
        (void) unsetenv("LOGNAME");

        /* It is needed otherwise cgroup creation fails */
        if (getuid() != 0) {
                puts("Skipping test: not root");
                return EXIT_TEST_SKIP;
        }

        r = enter_cgroup_subroot();
        if (r == -ENOMEDIUM) {
                puts("Skipping test: cgroupfs not available");
                return EXIT_TEST_SKIP;
        }

        assert_se(setenv("XDG_RUNTIME_DIR", "/tmp/", 1) == 0);
        assert_se(set_unit_path(get_testdata_dir("/test-execute")) >= 0);

        /* Unset VAR1, VAR2 and VAR3 which are used in the PassEnvironment test
         * cases, otherwise (and if they are present in the environment),
         * `manager_default_environment` will copy them into the default
         * environment which is passed to each created job, which will make the
         * tests that expect those not to be present to fail.
         */
        assert_se(unsetenv("VAR1") == 0);
        assert_se(unsetenv("VAR2") == 0);
        assert_se(unsetenv("VAR3") == 0);

        r = run_tests(UNIT_FILE_USER, user_tests);
        if (r != 0)
                return r;

        return run_tests(UNIT_FILE_SYSTEM, system_tests);
}
