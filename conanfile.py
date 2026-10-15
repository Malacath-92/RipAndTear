from conan import ConanFile


class ProxyPDF(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    def requirements(self):
        self.requires("structopt/0.1.3")
        self.requires("fmt/12.2.0")
        self.requires("magic_enum/0.9.2")

    def configure(self):
        pass
