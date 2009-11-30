Name:		cld
Version:	0.3
Release:	0.6.g50fcd615%{?dist}
Summary:	Coarse locking daemon

Group:		System Environment/Base
License:	GPLv2
URL:		http://hail.wiki.kernel.org/

# pulled from upstream git, commit 50fcd61590984c3c12f5584069da98bc8f4aec0f
# to recreate tarball, check out commit, then run "make dist"
Source0:	cld-%{version}git.tar.gz
Source2:	cld.init
Source3:	cld.sysconf
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

Requires(post):		chkconfig
Requires(preun):	chkconfig initscripts

BuildRequires:	db4-devel glib2-devel doxygen openssl-devel
BuildRequires:	texlive-latex

%description
Coarse locking daemon for cloud computing.  This software provides
a cache-coherent, highly-available distributed filesystem for small
files.

CLD's primary uses include consensus service (election of a master,
with fail-over, also known as lock service), reliable name space,
and reliable small file storage.

%package devel
Summary: Development files for %{name}
Group: Development/Libraries
Requires: %{name} = %{version}-%{release}
Requires: pkgconfig

%description devel
The %{name}-devel package contains libraries and header files for
developing applications that use %{name}.

%prep
%setup -q -n cld-0.3git

%build
%configure --disable-static
make %{?_smp_mflags}
rm -rf gendoc && mkdir gendoc && doxygen
( cd gendoc/latex && make )

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}

mkdir -p %{buildroot}%{_initddir}
install -m 755 %{SOURCE2} %{buildroot}%{_initddir}/cld

mkdir -p %{buildroot}%{_sysconfdir}/sysconfig
install -m 644 %{SOURCE3} %{buildroot}%{_sysconfdir}/sysconfig/cld

find %{buildroot} -name '*.la' -exec rm -f {} ';'

%check
make -s check

%clean
rm -rf %{buildroot}

%post
/sbin/ldconfig
# must be in chkconfig on
/sbin/chkconfig --add cld

%preun
if [ "$1" = 0 ] ; then
	/sbin/service cld stop >/dev/null 2>&1 ||:
	/sbin/chkconfig --del cld
fi

%postun
/sbin/ldconfig
if [ "$1" -ge "1" ]; then
	/sbin/service cld condrestart >/dev/null 2>&1 ||:
fi

%files
%defattr(-,root,root,-)
%doc AUTHORS COPYING LICENSE README NEWS doc/*.txt
%{_sbindir}/cld
%{_sbindir}/cldbadm
%{_bindir}/cldcli
%{_libdir}/*.so.*
%attr(0755,root,root)	%{_initddir}/cld
%config(noreplace)	%{_sysconfdir}/sysconfig/cld

%files devel
%defattr(-,root,root,-)
%doc gendoc/html gendoc/latex/refman.pdf
%{_libdir}/lib*.so
%{_libdir}/pkgconfig/*
%{_includedir}/*

%changelog
* Mon Nov 30 2009 Jeff Garzik <jgarzik@redhat.com> - 0.3-0.6.g50fcd615
- update to 0.3git commit 50fcd61590984c3c12f5584069da98bc8f4aec0f

* Sun Nov 29 2009 Jeff Garzik <jgarzik@redhat.com> - 0.3-0.5.g70268c35
- update to 0.3git commit 70268c3568db1e024f8adcdc399cbe235832ea17

* Sun Nov 15 2009 Jeff Garzik <jgarzik@redhat.com> - 0.3-0.4.g3cbe645c
- update to 0.3git commit 3cbe645cba84ca39973b85889d49de908b4536be

* Wed Nov  5 2009 Jeff Garzik <jgarzik@redhat.com> - 0.3-0.3.g3fc1d60a
- update to 0.3git commit 3fc1d60ab0791cdfa0c31df486aa9e33e952bd6a

* Wed Sep 30 2009 Jeff Garzik <jgarzik@redhat.com> - 0.3-0.2.gaaa8fb15
- update to 0.3git commit aaa8fb15a4345cb1da65869e51691ff33549639c

* Tue Sep 29 2009 Jeff Garzik <jgarzik@redhat.com> - 0.3-0.1.g232df337
- update to 0.3git commit 232df337e0063a64315aff67073e1fd69db2b19a

* Thu Aug 27 2009 Tomas Mraz <tmraz@redhat.com> - 0.2.1-2
- rebuilt with new openssl

* Wed Aug 26 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2.1-1
- Upstream version 0.2.1 release (== many bug fixes).

* Sat Aug 15 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-1
- Upstream version 0.2 release.

* Fri Aug 14 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.17.gbb117496
- update to commit bb11749606a204b9ab87b815352d231179dba7a1

* Thu Aug 13 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.16.gefe1f957
- update to commit efe1f95712cefa78cf28c328bcce8e25ac50e72b

* Wed Aug 12 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.15.g6409c437
- update to commit 6409c437045a4545377c17016d514ecaca3d74a0

* Tue Aug 11 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.14.g8c350253
- update to commit 8c3502530c8061ffdaf892e9529abceecfee8dcf

* Sat Aug  8 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.12.g85e14281
- update to commit 85e14281e1638ed8417c71beab0ba9361a9f0d2d

* Fri Aug  7 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.11.gf830b4be
- update to commit f830b4be92f89ad0a48ee0703fd1b829febfaf4a
  (major network protocol change)

* Wed Jul 29 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.10.gdb750ce5
- install cldcli

* Wed Jul 29 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.9.gdb750ce5
- update to commit db750ce53f6e50dae361e746d712a3785124f7a0

* Fri Jul 24 2009 Fedora Release Engineering <rel-eng@lists.fedoraproject.org> - 0.2-0.8.g487d5fb5
- Rebuilt for https://fedoraproject.org/wiki/Fedora_12_Mass_Rebuild

* Thu Jul 23 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.7.g487d5fb5
- update to commit 487d5fb50be8275a0e0cd36a882acdf1afe9a922
- require chkconfig, initscripts per pkg guidelines

* Thu Jul 23 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.6.g2a5e626a
- update to commit 2a5e626aa6e08d894e74af053236947cced8ff87
  to fix koji-related 'make check' logging issues.

* Tue Jul 21 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.5.g988e17d1
- update to commit 988e17d1b0ad8eef6df3f6f237e261d388adff59
- remove ExcludeArch

* Tue Jul 21 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.4.gc5b5f962
- rebuild for koji silliness

* Tue Jul 21 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.3.gc5b5f962
- update to commit c5b5f9622334b273c47e7aad5bd53e280041a045

* Sun Jul 19 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.2.g023a127d
- improve package description
- per guidelines, indicate how to regenerate tarball from git repo

* Fri Jul 17 2009 Jeff Garzik <jgarzik@redhat.com> - 0.2-0.1.g023a127d
- update with new release version scheme
- kill RPM_BUILD_ROOT

* Fri Jul 17 2009 Jeff Garzik <jgarzik@redhat.com> - 0.1-1
- version 0.1 release

* Thu Jul 16 2009 Jeff Garzik <jgarzik@redhat.com> - 0.1git-5
- chkconfig default off
- add doc: COPYING, LICENSE
- config(noreplace) sysconfig/cld

* Thu Jul 16 2009 Jeff Garzik <jgarzik@redhat.com> - 0.1git-4
- minor spec updates for review feedback, Fedora packaging guidelines

* Thu Jul 16 2009 Jeff Garzik <jgarzik@redhat.com> - 0.1git-3
- update BuildRequires
- rpmlint fixes
- update to latest git repo

* Wed Mar 18 2009 Jeff Garzik <jgarzik@redhat.com> - 0.1git-2
- update cld initscript

* Wed Mar 18 2009 Jeff Garzik <jgarzik@redhat.com> - 0.1git-1.libtest
- initial release

