if bashcompletiondir != 'no'
  aliases = []
<<<<<<< HEAD

  if get_option('daemon')
    aliases += [
      'pacmd',
      'pasuspender',
    ]
  endif

  aliases += [
    'pacat',
    'pactl',
    'padsp',
    'paplay',
    'parec',
    'parecord',
  ]
=======
>>>>>>> c1990dd02647405b0c13aab59f75d05cbb202336

  if get_option('daemon')
    aliases += [
      'pulseaudio',
      'pacmd',
      'pasuspender',
    ]

    # Create target directory for symlinks
    if meson.version().version_compare('>= 0.60.0')
      install_emptydir(bashcompletiondir)
    else
      meson.add_install_script('sh', '-c', 'mkdir -p $DESTDIR@0@'.format(bashcompletiondir))
    endif
  endif

  if get_option('client')
    aliases += [
      'pacat',
      'padsp',
      'paplay',
      'parec',
      'parecord',
    ]
    install_data('pactl', install_dir : bashcompletiondir)
  endif

  foreach alias : aliases
    dst = join_paths(bashcompletiondir, alias)
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pactl', dst)
    meson.add_install_script('sh', '-c', cmd)
  endforeach
endif
