# You need to prepare a configuration object with the required metadata
packaging.set ('arch', target_machine.cpu_family ())
packaging.set ('system', target_machine.system ())
configure_file (input: 'meta.in', output: 'meta', configuration: packaging)

# RPM is awful and I've given up on both manual generation (we'd have to either
# include rpmrc data or generate fake noarch packages) and rpmbuild (just no)
run_target ('deb',
	command: [join_paths (meson.current_source_dir (), 'make-deb.sh')])
run_target ('pacman',
	command: [join_paths (meson.current_source_dir (), 'make-pacman.sh')])
