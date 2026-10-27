"""Shared plot export with persistent synthetic-data identification."""
def save_figure(fig, path, validation=False, **kwargs):
    if validation:
        fig.text(.5,.5,'SYNTHETIC VALIDATION ONLY',ha='center',va='center',
                 fontsize=min(28,fig.get_figwidth()*2.4),color='crimson',alpha=.25,rotation=25,zorder=1000,
                 bbox={'facecolor':'white','alpha':.35,'edgecolor':'none'})
    fig.savefig(path,**kwargs)
