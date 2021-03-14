workspace(name = "com_github_bazelboost_thread")

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

git_repository(
    name = "boost",
    remote = "https://github.com/bazelboost/boost",
    branch = "master",
)

load("@boost//:index.bzl", "boost_git_repositories")
boost_git_repositories(branch = "develop")
