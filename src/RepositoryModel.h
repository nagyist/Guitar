#ifndef REPOSITORYMODEL_H
#define REPOSITORYMODEL_H

#include "BranchLabel.h"
#include "GitCommandCache.h"
#include "GitObjectManager.h"

typedef QList<Git::Tag> TagList;
typedef QList<Git::Branch> BranchList;
typedef QList<BranchLabel> BranchLabelList;

struct RepositoryData {
	std::mutex *mutex_ = nullptr;

	Git::CommitItemList commit_log;
	std::map<Git::Hash, BranchList> branch_map;
	std::map<Git::Hash, TagList> tag_map;
	std::map<int, BranchLabelList> label_map;

	std::map<QString, Git::Diff> diff_cache;
	GitObjectCache object_cache;

	GitCommandCache git_command_cache;

	RepositoryData(std::mutex *mutex)
		: mutex_(mutex)
		, object_cache(mutex)
	{}
};

struct CommitLogExchangeData {
	struct D {
		std::optional<Git::CommitItemList> commit_log;
		std::optional<std::map<Git::Hash, BranchList>> branch_map;
		std::optional<std::map<Git::Hash, TagList>> tag_map;
		std::optional<std::map<int, BranchLabelList>> label_map;
	};
	std::shared_ptr<D> p;
	CommitLogExchangeData()
		: p(std::make_shared<D>())
	{}
	CommitLogExchangeData(CommitLogExchangeData const &r)
		: p(std::make_shared<D>(*r.p))
	{}
};
Q_DECLARE_METATYPE(CommitLogExchangeData)

#endif // REPOSITORYMODEL_H
